#include "pch.h"
#include "IListView.h"
#include "ListViewHelper.h"

bool ListViewHelper::SaveAll(PCWSTR path, CListViewCtrl& lv, bool includeHeaders) {
	wil::unique_handle hFile(::CreateFile(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
	if (!hFile)
		return false;

	auto count = lv.GetItemCount();
	auto header = lv.GetHeader();
	auto columns = header.GetItemCount();
	CString text;
	DWORD written;

	if (includeHeaders) {
		HDITEM hdi;
		WCHAR text[64] = { 0 };
		hdi.cchTextMax = _countof(text);
		hdi.pszText = text;
		hdi.mask = HDI_TEXT;
		for (int i = 0; i < columns; i++) {
			header.GetItem(i, &hdi);
			::wcscat_s(text, i == columns - 1 ? L"\n" : L"\t");
			::WriteFile(hFile.get(), text, (DWORD)::wcslen(text) * sizeof(WCHAR), &written, nullptr);
		}
	}

	for (int i = 0; i < count; i++) {
		for (int c = 0; c < columns; c++) {
			text.Empty();
			lv.GetItemText(i, c, text);
			text += c == columns - 1 ? L"\n" : L"\t";
			::WriteFile(hFile.get(), text.GetBuffer(), text.GetLength() * sizeof(WCHAR), &written, nullptr);
		}
	}

	return true;
}

CString ListViewHelper::GetRowAsString(CListViewCtrl& lv, int row, WCHAR separator) {
	CString text, item;
	auto count = lv.GetHeader().GetItemCount();
	for (int c = 0; c < count; c++) {
		lv.GetItemText(row, c, item);
		text += item;
		if(c < count - 1)
			text += separator;
	}
	return text;
}

IListView* ListViewHelper::GetIListView(HWND hListView) {
	IListView* p{ nullptr };
	::SendMessage(hListView, LVM_QUERYINTERFACE, reinterpret_cast<WPARAM>(&__uuidof(IListView)), reinterpret_cast<LPARAM>(&p));
	return p;
}
