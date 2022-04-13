#include "pch.h"
#include "ObjectManager.h"
#include "DriverHelper.h"
#include "NtDll.h"
#include <VersionHelpers.h>

#pragma pack(push, 1)
typedef struct _GDI_HANDLE_ENTRY {
	union {
		PVOID Object;
		PVOID NextFree;
	};
	union {
		struct {
			USHORT ProcessId;
			USHORT Lock : 1;
			USHORT Count : 15;
		};
		ULONG Value;
	} Owner;
	USHORT Unique;
	UCHAR Type;
	UCHAR Flags;
	PVOID UserPointer;
} GDI_HANDLE_ENTRY, * PGDI_HANDLE_ENTRY;
#pragma pack(pop)

static_assert(sizeof(GDI_HANDLE_ENTRY) == 24);

#define GDI_HANDLE_INDEX_BITS 16
#define GDI_MAKE_HANDLE(Index, Unique) ((ULONG)(((ULONG)(Unique) << GDI_HANDLE_INDEX_BITS) | (ULONG)(Index)))

#define STATUS_INFO_LENGTH_MISMATCH      ((NTSTATUS)0xC0000004L)

std::vector<std::shared_ptr<ObjectTypeInfo>> ObjectManager::_types;
std::unordered_map<int16_t, std::shared_ptr<ObjectTypeInfo>> ObjectManager::_typesMap;
std::unordered_map<std::wstring, std::shared_ptr<ObjectTypeInfo>> ObjectManager::_typesNameMap;
std::vector<ObjectManager::Change> ObjectManager::_changes;
int64_t ObjectManager::_totalHandles;
int64_t ObjectManager::_totalObjects;

int ObjectManager::EnumTypes() {
	const ULONG len = 1 << 14;
	BYTE buffer[len];
	if (!NT_SUCCESS(NT::NtQueryObject(nullptr, NT::ObjectTypesInformation, buffer, len, nullptr)))
		return 0;

	auto p = reinterpret_cast<NT::OBJECT_TYPES_INFORMATION*>(buffer);
	bool empty = _types.empty();

	auto count = p->NumberOfTypes;
	if (empty) {
		_types.reserve(count);
		_typesMap.reserve(count);
		_changes.reserve(32);
	}
	else {
		_changes.clear();
		ATLASSERT(count == _types.size());
	}
	auto raw = &p->TypeInformation[0];
	_totalHandles = _totalObjects = 0;

	for (ULONG i = 0; i < count; i++) {
		if (!IsWindows8OrGreater()) {
			// TypeIndex is only supported since Win8. Uses the fake index for previous OS.
			raw->TypeIndex = static_cast<decltype(raw->TypeIndex)>(i);
		}
		auto type = empty ? std::make_shared<ObjectTypeInfo>() : _typesMap[raw->TypeIndex];
		if (empty) {
			type->GenericMapping = raw->GenericMapping;
			type->TypeIndex = raw->TypeIndex;
			type->DefaultNonPagedPoolCharge = raw->DefaultNonPagedPoolCharge;
			type->DefaultPagedPoolCharge = raw->DefaultPagedPoolCharge;
			type->TypeName = CString(raw->TypeName.Buffer, raw->TypeName.Length / sizeof(WCHAR));
			type->PoolType = (PoolType)raw->PoolType;
			type->DefaultNonPagedPoolCharge = raw->DefaultNonPagedPoolCharge;
			type->DefaultPagedPoolCharge = raw->DefaultPagedPoolCharge;
			type->ValidAccessMask = raw->ValidAccessMask;
			type->InvalidAttributes = raw->InvalidAttributes;
		}
		else {
			if (type->TotalNumberOfHandles != raw->TotalNumberOfHandles)
				_changes.push_back({ type, ChangeType::TotalHandles, (int32_t)raw->TotalNumberOfHandles - (int32_t)type->TotalNumberOfHandles });
			if (type->TotalNumberOfObjects != raw->TotalNumberOfObjects)
				_changes.push_back({ type, ChangeType::TotalObjects, (int32_t)raw->TotalNumberOfObjects - (int32_t)type->TotalNumberOfObjects });
			if (type->HighWaterNumberOfHandles != raw->HighWaterNumberOfHandles)
				_changes.push_back({ type, ChangeType::PeakHandles, (int32_t)raw->HighWaterNumberOfHandles - (int32_t)type->HighWaterNumberOfHandles });
			if (type->HighWaterNumberOfObjects != raw->HighWaterNumberOfObjects)
				_changes.push_back({ type, ChangeType::PeakObjects, (int32_t)raw->HighWaterNumberOfObjects - (int32_t)type->HighWaterNumberOfObjects });
		}

		type->TotalNumberOfHandles = raw->TotalNumberOfHandles;
		type->TotalNumberOfObjects = raw->TotalNumberOfObjects;
		type->TotalNonPagedPoolUsage = raw->TotalNonPagedPoolUsage;
		type->TotalPagedPoolUsage = raw->TotalPagedPoolUsage;
		type->HighWaterNumberOfObjects = raw->HighWaterNumberOfObjects;
		type->HighWaterNumberOfHandles = raw->HighWaterNumberOfHandles;
		type->TotalNamePoolUsage = raw->TotalNamePoolUsage;

		_totalObjects += raw->TotalNumberOfObjects;
		_totalHandles += raw->TotalNumberOfHandles;

		if (empty) {
			_types.emplace_back(type);
			_typesMap.insert({ type->TypeIndex, type });
			_typesNameMap.insert({ std::wstring(type->TypeName), type });
		}

		auto temp = (BYTE*)raw + sizeof(NT::OBJECT_TYPE_INFORMATION) + raw->TypeName.MaximumLength;
		temp += sizeof(PVOID) - 1;
		raw = reinterpret_cast<NT::OBJECT_TYPE_INFORMATION*>((ULONG_PTR)temp / sizeof(PVOID) * sizeof(PVOID));
	}
	return static_cast<int>(_types.size());
}

bool ObjectManager::EnumHandlesAndObjects(PCWSTR type, DWORD pid, PCWSTR prefix, bool namedOnly) {
	EnumTypes();

	ULONG len = 1 << 25;
	std::unique_ptr<BYTE[]> buffer;
	do {
		buffer = std::make_unique<BYTE[]>(len);
		auto status = NT::NtQuerySystemInformation(NT::SystemExtendedHandleInformation, buffer.get(), len, &len);
		if (status == STATUS_INFO_LENGTH_MISMATCH) {
			len <<= 1;
			continue;
		}
		if (status == 0)
			break;
		return false;
	} while (true);

	auto filteredTypeIndex = type == nullptr || ::wcslen(type) == 0 ? -1 : _typesNameMap.at(type)->TypeIndex;

	auto p = (NT::SYSTEM_HANDLE_INFORMATION_EX*)buffer.get();
	auto count = p->NumberOfHandles;
	_objects.clear();
	_objectsByAddress.clear();
	_objects.reserve(count / 2);
	_objectsByAddress.reserve(count / 2);

	CString sprefix(prefix ? prefix : L"");

	auto& handles = p->Handles;
	for (decltype(count) i = 0; i < count; i++) {
		auto& handle = handles[i];
		if (filteredTypeIndex >= 0 && handle.ObjectTypeIndex != filteredTypeIndex)
			continue;

		if (pid && handle.UniqueProcessId != pid)
			continue;

		auto name = GetObjectName((HANDLE)handle.HandleValue, (ULONG)handle.UniqueProcessId, handle.ObjectTypeIndex);
		if (prefix) {
			if (name.IsEmpty() || name.Left(sprefix.GetLength()).CompareNoCase(sprefix) != 0)
				continue;
		}
		if (namedOnly && name.IsEmpty())
			continue;

		auto hi = std::make_shared<HandleInfo>();
		hi->HandleValue = (ULONG)handle.HandleValue;
		hi->GrantedAccess = handle.GrantedAccess;
		hi->Object = handle.Object;
		hi->HandleAttributes = handle.HandleAttributes;
		hi->ProcessId = (ULONG)handle.UniqueProcessId;
		hi->ObjectTypeIndex = handle.ObjectTypeIndex;
		if (auto it = _objectsByAddress.find(handle.Object); it == _objectsByAddress.end()) {
			auto obj = std::make_shared<ObjectInfo>();
			obj->HandleCount = 1;
			obj->Object = handle.Object;
			obj->Handles.push_back(hi);
			obj->TypeIndex = handle.ObjectTypeIndex;
			obj->TypeName = GetType(obj->TypeIndex)->TypeName;
			hi->ObjectInfo = obj.get();
			if(!name.IsEmpty())
				obj->Name = name;

			_objects.push_back(obj);
			_objectsByAddress.insert({ handle.Object, obj });
		}
		else {
			hi->ObjectInfo = nullptr;
			it->second->HandleCount++;
			it->second->Handles.push_back(hi);
		}
		_handles.push_back(hi);
	}

	return true;
}

bool ObjectManager::EnumHandles(PCWSTR type, DWORD pid, bool namedObjectsOnly) {
	EnumTypes();

	ULONG len = 1 << 25;
	std::unique_ptr<BYTE[]> buffer;
	do {
		buffer = std::make_unique<BYTE[]>(len);
		auto status = NT::NtQuerySystemInformation(NT::SystemExtendedHandleInformation, buffer.get(), len, &len);
		if (status == STATUS_INFO_LENGTH_MISMATCH) {
			len <<= 1;
			continue;
		}
		if (status == 0)
			break;
		return false;
	} while (true);

	auto filteredTypeIndex = type == nullptr || ::wcslen(type) == 0 ? -1 : _typesNameMap.at(type)->TypeIndex;

	auto p = (NT::SYSTEM_HANDLE_INFORMATION_EX*)buffer.get();
	auto count = p->NumberOfHandles;
	_handles.clear();
	_handles.reserve(count);
	for (decltype(count) i = 0; i < count; i++) {
		auto& handle = p->Handles[i];
		if (pid && handle.UniqueProcessId != pid)
			continue;

		if (filteredTypeIndex >= 0 && handle.ObjectTypeIndex != filteredTypeIndex)
			continue;

		// skip System Explorer process?
		if (_skipThisProcess && handle.UniqueProcessId == ::GetCurrentProcessId())
			continue;

		CString name;
		if (namedObjectsOnly && (name = GetObjectName((HANDLE)handle.HandleValue, (DWORD)handle.UniqueProcessId, handle.ObjectTypeIndex)).IsEmpty())
			continue;

		auto hi = std::make_shared<HandleInfo>();
		hi->HandleValue = (ULONG)handle.HandleValue;
		hi->GrantedAccess = handle.GrantedAccess;
		hi->Object = handle.Object;
		hi->HandleAttributes = handle.HandleAttributes;
		hi->ProcessId = (ULONG)handle.UniqueProcessId;
		hi->ObjectTypeIndex = handle.ObjectTypeIndex;
		hi->Name = name;

		_handles.emplace_back(hi);
	}

	return true;
}

const std::vector<std::shared_ptr<ObjectInfo>>& ObjectManager::GetObjects() const {
	return _objects;
}

const std::vector<ObjectManager::Change>& ObjectManager::GetChanges() const {
	return _changes;
}

std::vector<ObjectNameAndType> ObjectManager::EnumDirectoryObjects(PCWSTR path) {
	std::vector<ObjectNameAndType> objects;
	wil::unique_handle hDirectory;
	OBJECT_ATTRIBUTES attr;
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, path);
	InitializeObjectAttributes(&attr, &name, 0, nullptr, nullptr);
	if(!NT_SUCCESS(NT::NtOpenDirectoryObject(hDirectory.addressof(), DIRECTORY_QUERY, &attr)))
		return objects;

	objects.reserve(128);
	BYTE buffer[1 << 12];
	auto info = reinterpret_cast<NT::OBJECT_DIRECTORY_INFORMATION*>(buffer);
	bool first = true;
	ULONG size, index = 0;
	for(;;) {
		auto start = index;
		if (!NT_SUCCESS(NT::NtQueryDirectoryObject(hDirectory.get(), info, sizeof(buffer), FALSE, first, &index, &size)))
			break;
		first = false;
		for (ULONG i = 0; i < index - start; i++) {
			ObjectNameAndType data;
			auto& p = info[i];
			data.Name = std::wstring(p.Name.Buffer, p.Name.Length / sizeof(WCHAR));
			data.TypeName = std::wstring(p.TypeName.Buffer, p.TypeName.Length / sizeof(WCHAR));

			objects.push_back(std::move(data));
		}
	}
	return objects;
}

CString ObjectManager::GetSymbolicLinkTarget(PCWSTR path) {
	wil::unique_handle hLink;
	OBJECT_ATTRIBUTES attr;
	CString target;
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, path);
	InitializeObjectAttributes(&attr, &name, 0, nullptr, nullptr);
	if (NT_SUCCESS(NT::NtOpenSymbolicLinkObject(hLink.addressof(), GENERIC_READ, &attr))) {
		WCHAR buffer[1 << 10];
		UNICODE_STRING result;
		result.Buffer = buffer;
		result.MaximumLength = sizeof(buffer);
		if(NT_SUCCESS(NT::NtQuerySymbolicLinkObject(hLink.get(), &result, nullptr)))
			target.SetString(result.Buffer, result.Length / sizeof(WCHAR));
	}
	return target;
}

HANDLE ObjectManager::DupHandle(ObjectInfo * pObject, ACCESS_MASK access) {
	for (auto& h : pObject->Handles) {
		auto hDup = DriverHelper::DupHandle(ULongToHandle(h->HandleValue), h->ProcessId, //access);
			_typesMap.at(h->ObjectTypeIndex)->ValidAccessMask);
		if (hDup)
			return hDup;
	}
	return nullptr;
}

HANDLE ObjectManager::DupHandle(HANDLE h, DWORD pid, USHORT type, ACCESS_MASK access, DWORD flags) {
	auto hDup = DriverHelper::DupHandle(h, pid, _typesMap.at(type)->ValidAccessMask, flags);
	return hDup;
}

NTSTATUS ObjectManager::OpenObject(PCWSTR path, PCWSTR typeName, HANDLE* pHandle, DWORD access) {
	ATLASSERT(pHandle);
	if (pHandle == nullptr)
		return STATUS_INVALID_PARAMETER;

	//auto hObject = DriverHelper::OpenHandle(path, access);
	//if (hObject) {
	//	*pHandle = hObject;
	//	return STATUS_SUCCESS;
	//}

	HANDLE hObject = nullptr;
	CString type(typeName);
	OBJECT_ATTRIBUTES attr;
	UNICODE_STRING uname;
	RtlInitUnicodeString(&uname, path);
	InitializeObjectAttributes(&attr, &uname, 0, nullptr, nullptr);
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	if (type == L"Event")
		status = NT::NtOpenEvent(&hObject, access, &attr);
	else if (type == L"Mutant")
		status = NT::NtOpenMutant(&hObject, access, &attr);
	else if (type == L"Section")
		status = NT::NtOpenSection(&hObject, access, &attr);
	else if (type == L"Semaphore")
		status = NT::NtOpenSemaphore(&hObject, access, &attr);
	else if (type == "EventPair")
		status = NT::NtOpenEventPair(&hObject, access, &attr);
	else if (type == L"IoCompletion")
		status = NT::NtOpenIoCompletion(&hObject, access, &attr);
	else if (type == L"SymbolicLink")
		status = NT::NtOpenSymbolicLinkObject(&hObject, access, &attr);
	else if (type == L"Key")
		status = NT::NtOpenKey(&hObject, access, &attr);
	else if (type == L"Job")
		status = NT::NtOpenJobObject(&hObject, access, &attr);
	else if (type == L"File" || type == L"Device") {
		IO_STATUS_BLOCK ioStatus;
		status = NT::NtOpenFile(&hObject, access, &attr, &ioStatus, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
	}

	*pHandle = hObject;
	return status;
}

int64_t ObjectManager::GetTotalHandles() {
	return _totalHandles;
}

int64_t ObjectManager::GetTotalObjects() {
	return _totalObjects;
}

std::vector<HWND> ObjectManager::EnumDsktopWindows(HDESK hDesktop) {
	std::vector<HWND> windows;
	windows.reserve(128);

	::EnumDesktopWindows((HDESK)hDesktop, [](auto h, auto p) {
		reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
		return TRUE;
		}, reinterpret_cast<LPARAM>(&windows));
	return windows;
}

std::vector<HWND> ObjectManager::EnumChildWindows(HWND hWnd) {
	std::vector<HWND> windows;
	::EnumChildWindows(hWnd, [](auto h, auto p) {
		reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
		return TRUE;
		}, reinterpret_cast<LPARAM>(&windows));
	return windows;
}

std::vector<GdiObject> ObjectManager::EnumGdiObjects(DWORD pid) {
	std::vector<GdiObject> objects;

	auto hProcess = ::GetCurrentProcess();

	PROCESS_BASIC_INFORMATION pbi;
	NT::NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
	auto const GdiSharedHandleTable = 0xf8;

	GDI_HANDLE_ENTRY* GdiAddress;
	SIZE_T read;
	if (!::ReadProcessMemory(hProcess, (BYTE*)pbi.PebBaseAddress + GdiSharedHandleTable, &GdiAddress, sizeof(GdiAddress), &read))
		return objects;

	unsigned tableSize = 64 << 10;
	auto table = std::make_unique<GDI_HANDLE_ENTRY[]>(tableSize);
	if (!::ReadProcessMemory(hProcess, GdiAddress, table.get(), tableSize * sizeof(GDI_HANDLE_ENTRY), &read))
		return objects;

	objects.reserve(128);
	for (unsigned i = 0; i < tableSize; i++) {
		auto obj = table.get() + i;
		if (obj->Owner.ProcessId == 0)
			continue;

		if (obj->Owner.ProcessId != pid)
			continue;

		GdiObject object;
		object.Object = obj->Object;
		object.ProcessId = obj->Owner.ProcessId;
		object.Index = (USHORT)i;
		object.Handle = GDI_MAKE_HANDLE(i, obj->Unique);
		object.Type = (GdiObjectType)(obj->Type & 0x7f);
		object.Count = obj->Owner.Count;
		objects.push_back(object);
	}

	return objects;
}

bool ObjectManager::GetObjectInfo(ObjectInfo* info, HANDLE hObject, ULONG pid, USHORT type) const {
	auto hDup = DupHandle(info);
	if (hDup) {
		info->Name = GetObjectName(hDup, type);
		::CloseHandle(hDup);
		return true;
	}
	return false;
}

CString ObjectManager::GetObjectName(HANDLE hObject, ULONG pid, USHORT type) const {
	auto hDup = DriverHelper::DupHandle(hObject, pid, 0);
	CString name;
	if(hDup) {
		name = GetObjectName(hDup, type);
		::CloseHandle(hDup);
	}
	return name;
}

CString ObjectManager::GetObjectName(HANDLE hDup, USHORT type) {
	static int processTypeIndex = _typesNameMap.find(L"Process")->second->TypeIndex;
	static int threadTypeIndex = _typesNameMap.find(L"Thread")->second->TypeIndex;
	static int fileTypeIndex = _typesNameMap.find(L"File")->second->TypeIndex;
	ATLASSERT(processTypeIndex > 0 && threadTypeIndex > 0);

	CString sname;
	do {
		if (type == processTypeIndex || type == threadTypeIndex)
			break;

		BYTE buffer[2048];
		if (type == fileTypeIndex) {
			// special case for files in case they're locked
			struct Data {
				HANDLE hDup;
				BYTE* buffer;
			} data = { hDup, buffer };

			wil::unique_handle hThread(::CreateThread(nullptr, 1 << 13, [](auto p) {
				auto d = (Data*)p;
				return (DWORD)NT::NtQueryObject(d->hDup, NT::ObjectNameInformation, d->buffer, sizeof(buffer), nullptr);
				}, &data, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
			if (::WaitForSingleObject(hThread.get(), 6) == WAIT_TIMEOUT) {
				::TerminateThread(hThread.get(), 1);
			}
			else {
				DWORD code;
				::GetExitCodeThread(hThread.get(), &code);
				if (code == STATUS_SUCCESS) {
					auto name = (NT::POBJECT_NAME_INFORMATION)buffer;
					sname = CString(name->Name.Buffer, name->Name.Length / sizeof(WCHAR));
				}
			}
		}
		else if (NT_SUCCESS(NT::NtQueryObject(hDup, NT::ObjectNameInformation, buffer, sizeof(buffer), nullptr))) {
			auto name = (NT::POBJECT_NAME_INFORMATION)buffer;
			sname = CString(name->Name.Buffer, name->Name.Length / sizeof(WCHAR));
		}
	} while (false);

	return sname;
}

std::shared_ptr<ObjectTypeInfo> ObjectManager::GetType(USHORT index) {
	return _typesMap.at(index);
}

std::shared_ptr<ObjectTypeInfo> ObjectManager::GetType(PCWSTR name) {
	return _typesNameMap.at(name);
}

const std::vector<std::shared_ptr<ObjectTypeInfo>>& ObjectManager::GetObjectTypes() {
	return _types;
}

const std::vector<std::shared_ptr<HandleInfo>>& ObjectManager::GetHandles() const {
	return _handles;
}

bool ObjectManager::GetStats(ObjectAndHandleStats& stats) {
	const ULONG len = 1 << 14;
	BYTE buffer[len];
	if (!NT_SUCCESS(NT::NtQueryObject(nullptr, NT::ObjectTypesInformation, buffer, len, nullptr)))
		return false;

	auto p = reinterpret_cast<NT::OBJECT_TYPES_INFORMATION*>(buffer);
	auto count = p->NumberOfTypes;
	::ZeroMemory(&stats, sizeof(stats));
	auto raw = &p->TypeInformation[0];

	for (ULONG i = 0; i < count; i++) {
		stats.TotalHandles += raw->TotalNumberOfHandles;
		stats.TotalObjects += raw->TotalNumberOfObjects;
		stats.PeakHandles += raw->HighWaterNumberOfHandles;
		stats.PeakObjects += raw->HighWaterNumberOfObjects;

		auto temp = (BYTE*)raw + sizeof(NT::OBJECT_TYPE_INFORMATION) + raw->TypeName.MaximumLength;
		temp += sizeof(PVOID) - 1;
		raw = reinterpret_cast<NT::OBJECT_TYPE_INFORMATION*>((ULONG_PTR)temp / sizeof(PVOID) * sizeof(PVOID));
	}
	return true;
}
