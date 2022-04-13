#include <ntifs.h>
#include <ntddk.h>
#include "KObjExp.h"

#define DRIVER_PREFIX "KObjExp"

DRIVER_UNLOAD ObjExpUnload;

DRIVER_DISPATCH ObjExpCreateClose, ObjExpDeviceControl;
//extern POBJECT_TYPE* ExWindowStationObjectType;

extern "C" POBJECT_TYPE ObGetObjectType(PVOID Object);

extern "C" NTSTATUS ZwOpenThread(
	_Out_ PHANDLE ThreadHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_opt_ PCLIENT_ID ClientId);

extern "C" NTSTATUS NTAPI ZwQueryInformationProcess(
	_In_ HANDLE ProcessHandle,
	_In_ PROCESSINFOCLASS ProcessInformationClass,
	_Out_writes_bytes_(ProcessInformationLength) PVOID ProcessInformation,
	_In_ ULONG ProcessInformationLength,
	_Out_opt_ PULONG ReturnLength
);

extern "C" NTSTATUS ObOpenObjectByName(
	_In_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_ POBJECT_TYPE ObjectType,
	_In_ KPROCESSOR_MODE AccessMode,
	_Inout_opt_ PACCESS_STATE AccessState,
	_In_opt_ ACCESS_MASK DesiredAccess,
	_Inout_opt_ PVOID ParseContext,
	_Out_ PHANDLE Handle);

extern "C" HANDLE NTAPI __win32kstub_NtUserOpenWindowStation(
	_In_ POBJECT_ATTRIBUTES attr,
	_In_ ACCESS_MASK access);

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING) {
	PDEVICE_OBJECT DeviceObject;
	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\KObjExp");
	auto status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint((DRIVER_PREFIX "Failed to create device object (0x%X)\n", status));
		return status;
	}

	UNICODE_STRING symName = RTL_CONSTANT_STRING(L"\\??\\KObjExp");
	status = IoCreateSymbolicLink(&symName, &devName);
	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(DeviceObject);
		KdPrint((DRIVER_PREFIX "Failed to create symbolic link (0x%X)\n", status));
		return status;
	}

	DriverObject->DriverUnload = ObjExpUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = ObjExpCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ObjExpDeviceControl;

	return status;
}

void ObjExpUnload(PDRIVER_OBJECT DriverObject) {
	UNICODE_STRING symName = RTL_CONSTANT_STRING(L"\\??\\KObjExp");
	IoDeleteSymbolicLink(&symName);
	IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS ObjExpCreateClose(PDEVICE_OBJECT, PIRP Irp) {
	auto status = STATUS_SUCCESS;
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	if (stack->MajorFunction == IRP_MJ_CREATE) {
		// verify it's System explorer client (very simple at the moment)
		HANDLE hProcess;
		status = ObOpenObjectByPointer(PsGetCurrentProcess(), OBJ_KERNEL_HANDLE, nullptr, 0, *PsProcessType, KernelMode, &hProcess);
		NT_ASSERT(NT_SUCCESS(status));
		if (NT_SUCCESS(status)) {
			UCHAR buffer[280] = { 0 };
			status = ZwQueryInformationProcess(hProcess, ProcessImageFileName, buffer, sizeof(buffer) - sizeof(WCHAR), nullptr);
			if (NT_SUCCESS(status)) {
				auto path = (UNICODE_STRING*)buffer;
				auto bs = wcsrchr(path->Buffer, L'\\');
				NT_ASSERT(bs);
				if(bs == nullptr || (0 != _wcsicmp(bs, L"\\SysExp.exe") && 0 != _wcsicmp(bs, L"\\ObjExp.exe")))
					status = STATUS_ACCESS_DENIED;
			}
			ZwClose(hProcess);
		}
	}
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, 0);
	return status;
}

NTSTATUS ObjExpDeviceControl(PDEVICE_OBJECT, PIRP Irp) {
	const auto& dic = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl;
	auto status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG len = 0;
	POBJECT_TYPE ObjectType = nullptr;

	switch (dic.IoControlCode) {
		case IOCTL_KOBJEXP_OPEN_OBJECT:
		{
			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.InputBufferLength < sizeof(OpenObjectData)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			if (dic.OutputBufferLength < sizeof(HANDLE)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			auto data = static_cast<OpenObjectData*>(Irp->AssociatedIrp.SystemBuffer);
			auto obj = data->Address;
			status = ObReferenceObjectByPointer(obj, data->Access, nullptr, KernelMode);
			if (!NT_SUCCESS(status))
				break;

			HANDLE hObject;
			status = ObOpenObjectByPointer(data->Address, 0, nullptr, data->Access,
				nullptr, KernelMode, &hObject);
			if (NT_SUCCESS(status)) {
				*static_cast<HANDLE*>(Irp->AssociatedIrp.SystemBuffer) = hObject;
				len = sizeof(HANDLE);
			}
			ObDereferenceObject(obj);
			break;
		}

		case IOCTL_KOBJEXP_DUP_HANDLE:
		{
			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.InputBufferLength < sizeof(DupHandleData)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			if (dic.OutputBufferLength < sizeof(HANDLE)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			const auto data = static_cast<DupHandleData*>(Irp->AssociatedIrp.SystemBuffer);

			HANDLE hProcess;
			OBJECT_ATTRIBUTES procAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(nullptr, OBJ_KERNEL_HANDLE);
			CLIENT_ID pid{};
			pid.UniqueProcess = UlongToHandle(data->SourcePid);
			status = ZwOpenProcess(&hProcess, PROCESS_DUP_HANDLE, &procAttributes, &pid);
			if (!NT_SUCCESS(status)) {
				KdPrint(("Failed to open process %d (0x%08X)\n", data->SourcePid, status));
				break;
			}

			HANDLE hTarget;
			status = ZwDuplicateObject(hProcess, ULongToHandle(data->Handle), NtCurrentProcess(),
				&hTarget, data->AccessMask, 0, data->Flags);
			ZwClose(hProcess);
			if (!NT_SUCCESS(status)) {
				KdPrint(("Failed to duplicate handle (0x%8X)\n", status));
				break;
			}

			*(HANDLE*)Irp->AssociatedIrp.SystemBuffer = hTarget;
			len = sizeof(HANDLE);
			break;
		}

		//case IOCTL_KOBJEXP_OPEN_WINSTA_BY_NAME:
		//	ObjectType = *ExWindowStationObjectType;
		case IOCTL_KOBJEXP_OPEN_EVENT_BY_NAME:
			ObjectType = *ExEventObjectType;
		case IOCTL_KOBJEXP_OPEN_DESKTOP_BY_NAME:
			ObjectType = *ExDesktopObjectType;
		case IOCTL_KOBJEXP_OPEN_SEMAPHORE_BY_NAME:
			if (ObjectType == nullptr)
				ObjectType = *ExSemaphoreObjectType;
		case IOCTL_KOBJEXP_OPEN_JOB_BY_NAME:
		{
			if (ObjectType == nullptr)
				ObjectType = *PsJobType;

			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.OutputBufferLength < sizeof(HANDLE)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			auto name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
			if (name[dic.InputBufferLength / sizeof(WCHAR) - 1] != L'\0') {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			OBJECT_ATTRIBUTES attr;
			UNICODE_STRING uname;
			RtlInitUnicodeString(&uname, name);
			InitializeObjectAttributes(&attr, &uname, 0, nullptr, nullptr);
			status = ObOpenObjectByName(&attr, ObjectType, KernelMode, nullptr, GENERIC_READ, nullptr, (HANDLE*)name);
			len = NT_SUCCESS(status) ? sizeof(HANDLE) : 0;
			break;
		}

		case IOCTL_KOBJEXP_OPEN_PROCESS:
		case IOCTL_KOBJEXP_OPEN_THREAD:
		{
			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.InputBufferLength < sizeof(OpenProcessThreadData) || dic.OutputBufferLength < sizeof(HANDLE)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			auto data = (OpenProcessThreadData*)Irp->AssociatedIrp.SystemBuffer;
			OBJECT_ATTRIBUTES attr = RTL_CONSTANT_OBJECT_ATTRIBUTES(nullptr, 0);
			CLIENT_ID id{};
			if (dic.IoControlCode == IOCTL_KOBJEXP_OPEN_PROCESS) {
				id.UniqueProcess = UlongToHandle(data->Id);
				status = ZwOpenProcess((HANDLE*)data, data->AccessMask, &attr, &id);
			}
			else {
				id.UniqueThread = UlongToHandle(data->Id);
				status = ZwOpenThread((HANDLE*)data, data->AccessMask, &attr, &id);
			}
			len = NT_SUCCESS(status) ? sizeof(HANDLE) : 0;
			break;
		}

		case IOCTL_KOBJEXP_GET_VERSION:
			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.OutputBufferLength < sizeof(USHORT)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			*(USHORT*)Irp->AssociatedIrp.SystemBuffer = DRIVER_CURRENT_VERSION;
			len = sizeof(USHORT);
			status = STATUS_SUCCESS;
			break;

		case IOCTL_KOBJEXP_GET_OBJECT_ADDRESS:
			if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			if (dic.InputBufferLength < sizeof(HANDLE) || dic.OutputBufferLength < sizeof(void*)) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			PVOID Object;
			status = ObReferenceObjectByHandle(*(HANDLE*)Irp->AssociatedIrp.SystemBuffer, GENERIC_READ, nullptr, KernelMode, &Object, nullptr);
			if (NT_SUCCESS(status)) {
				*(PVOID*)Irp->AssociatedIrp.SystemBuffer = Object;
				ObDereferenceObject(Object);
				len = sizeof(Object);
			}
			break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = len;
	IoCompleteRequest(Irp, 0);
	return status;
}
