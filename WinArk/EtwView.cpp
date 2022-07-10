#include "stdafx.h"
#include "EtwView.h"
#include "resource.h"
#include "TraceManager.h"
#include "FormatHelper.h"
#include <execution>
#include "ClipboardHelper.h"
#include "SortHelper.h"
#include "EventsDlg.h"
#include "FiltersDlg.h"
#include "FilterFactory.h"
#include "CallStackDlg.h"
#include "EventPropertiesDlg.h"
#include "SymbolManager.h"
#include "Helpers.h"
#include <filesystem>
#include "SymbolHelper.h"
#include <unordered_set>
#include <DriverHelper.h>
#include "QuickFindDlg.h"
#include "SerializerFactory.h"

CEtwView::CEtwView(IEtwFrame* frame) :CViewBase(frame) {
}

void CEtwView::Activate(bool active) {
	m_IsActive = active;
	if (active) {
		UpdateEventStatus();
		UpdateUI();
	}
}

void CEtwView::AddEvent(std::shared_ptr<EventData> data) {
	{
		std::lock_guard lock(m_EventsLock);
		m_TempEvents.push_back(data);
	}
}

void CEtwView::StartMonitoring(TraceManager& tm, bool start) {
	if (start) {
		std::vector<KernelEventTypes> types;
		std::vector<std::wstring> categories;
		for (auto& cat : m_EventsConfig.GetCategories()) {
			auto c = KernelEventCategory::GetCategory(cat.Name.c_str());
			ATLASSERT(c);
			types.push_back(c->EnableFlag);
			categories.push_back(c->Name);
		}
		std::initializer_list<KernelEventTypes> events(types.data(), types.data() + types.size());
		tm.SetKernelEventTypes(events);
		tm.SetKernelEventStacks(std::initializer_list<std::wstring>(categories.data(), categories.data() + categories.size()));
		ApplyFilters(m_FilterConfig);
	}
	else {
		m_IsDraining = true;
	}
	m_IsMonitoring = start;
}

CString CEtwView::GetColumnText(HWND, int row, int col) const {
	auto item = m_Events[row].get();

	CString text;
	switch (col)
	{
		case 0:// index
			text.Format(L"%7u", item->GetIndex());
			if (item->GetStackEventData())
				text += L" *";
			break;

		case 1:// Time
		{
			auto ts = item->GetTimeStamp();
			return FormatHelper::FormatTime(ts);
		}

		case 3:// PID
		{
			DWORD tid = -1;
			DWORD pid = -1;
			if (item->GetEventName() == L"Thread/CSwitch") {
				tid = item->GetProperty(L"NewThreadId")->GetValue<DWORD>();
				HANDLE hThread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
				if (hThread != NULL) {
					pid = GetProcessIdOfThread(hThread);
					::CloseHandle(hThread);
				}
			}
			else if (item->GetEventName() == L"PerfInfo/SysClEnter") {
				tid = _threadById[item->GetProcessorNumber()];
				pid = _processById[tid];
			}
			else {
				pid = item->GetProcessId();
			}
			if (pid != (DWORD)-1 && pid != 0)
				text.Format(L"%u (0x%X)", pid, pid);
			break;
		}

		case 5:// TID
		{
			DWORD tid = -1;
			if (item->GetEventName() == L"Thread/CSwitch") {
				tid = item->GetProperty(L"NewThreadId")->GetValue<DWORD>();
			}
			else if (item->GetEventName() == L"PerfInfo/SysClEnter") {
				tid = _threadById[item->GetProcessorNumber()];
			}
			else {
				tid = item->GetThreadId();
			}
			if (tid != (DWORD)-1 && tid != 0)
				text.Format(L"%u (0x%X)", tid, tid);

			break;
		}

		case 6:// Details
			return GetEventDetails(item).c_str();
	}

	return text;
}

int CEtwView::GetRowImage(HWND, int row) const {
	auto& evt = m_Events[row];
	if (auto it = s_IconsMap.find(evt->GetEventName()); it != s_IconsMap.end())
		return it->second;

	auto pos = evt->GetEventName().find(L'/');
	if (pos != std::wstring::npos) {
		if (auto it = s_IconsMap.find(evt->GetEventName().substr(0, pos)); it != s_IconsMap.end())
			return it->second;
	}
	return 0;
}

PCWSTR CEtwView::GetColumnTextPointer(HWND, int row, int col) const {
	auto item = m_Events[row].get();
	switch (col)
	{
		case 2: return item->GetEventName().c_str();
		case 4: 
			if (item->GetEventName() == L"PerfInfo/SysClEnter") {
				DWORD tid = _threadById[item->GetProcessorNumber()];
				DWORD pid = _processById[tid];
				return _processNameById[pid].c_str();
			}
			return item->GetProcessName().c_str();
	}
	return nullptr;
}

bool CEtwView::OnRightClickList(HWND, int index, int col, POINT& pt) {
	if (index >= 0) {
		CMenu menu;
		menu.LoadMenu(IDR_ETW_CONTEXT);
		GetFrame()->TrackPopupMenu(menu.GetSubMenu(0), *this, &pt);
		return true;
	}
	return false;
}

bool CEtwView::OnDoubleClickList(HWND, int row, int col, POINT& pt) {
	SendMessage(WM_COMMAND, ID_EVENT_PROPERTIES);
	return true;
}

DWORD CEtwView::AddNewProcess(DWORD tid) const{
	DWORD pid = -1;
	HANDLE hThread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
	if (hThread != NULL) {
		pid = GetProcessIdOfThread(hThread);
		::CloseHandle(hThread);
	}
	if (pid != -1&&pid!=0) {
		std::unique_ptr<WinSys::Process> process;
		_processById.insert({ tid,pid });
		auto hProcess = DriverHelper::OpenProcess(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
		if (!hProcess)
			hProcess = DriverHelper::OpenProcess(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ);
		if (hProcess) {
			process.reset(new WinSys::Process(hProcess));
			std::wstring procName = process->GetName();
			_processNameById.insert({ pid,procName });
		}
	}
	return pid;
}

std::wstring CEtwView::ProcessSpecialEvent(EventData* data) const {
	std::wstring details;
	CString text;
	auto& name = data->GetEventName();
	if (name == L"Process/Start") {
		text.Format(L"PID: %u; Image: %s; Command Line: %s",
			data->GetProperty(L"ProcessId")->GetValue<DWORD>(),
			CString(data->GetProperty(L"ImageFileName")->GetAnsiString()),
			data->GetProperty(L"CommandLine")->GetUnicodeString());
		details = std::move(text);
	}
	if (name == L"PerfInfo/SysClEnter") {
		DWORD64 address;
		auto prop = data->GetProperty(L"SysCallAddress");
		if (prop->GetLength() == 4)
			address = prop->GetValue<DWORD>();
		else
			address = prop->GetValue<DWORD64>();
		auto& symbols = SymbolHelper::Get();
		DWORD64 offset = 0;
		auto symbol = symbols.GetSymbolFromAddress(address, &offset);
		if (symbol) {
			auto sym = symbol->GetSymbolInfo();
			CStringA text;
			text.Format("%s", sym->Name);
			details = Helpers::StringToWstring(std::string(text));
		}
	}
	if (name == L"Thread/CSwitch") {
		UCHAR processorNumber = data->GetProcessorNumber();
		DWORD tid = data->GetProperty(L"NewThreadId")->GetValue<DWORD>();
		if (tid != -1) {
			_threadById[processorNumber] = tid;
		}
		if (_processById.count(tid) == 0) {
			DWORD pid = AddNewProcess(tid);
			if (pid != -1 && pid != 0) {
				data->SetProcessId(pid);
			}
		}
		tid = data->GetProperty(L"OldThreadId")->GetValue<DWORD>();
		if (_processById.count(tid) == 0) {
			AddNewProcess(tid);
		}
		auto state = data->GetProperty(L"OldThreadState")->GetValue<uint8_t>();
		if (_processById.count(tid) != 0 && state == (uint8_t)WinSys::ThreadState::Terminated) {
			if (_processNameById.count(_processById[tid]) != 0) {
				_processNameById.erase(_processById[tid]);
			}
			_processById.erase(tid);
		}
	}
	return details;
}

std::wstring CEtwView::GetEventDetails(EventData* data) const {
	auto details = ProcessSpecialEvent(data);
	if (details.empty()) {
		for (auto& prop : data->GetProperties()) {
			if (prop.Name.substr(0, 8) != L"Reserved" && prop.Name.substr(0, 4) != L"TTID") {
				auto value = FormatHelper::FormatProperty(data, prop);
				if (value.empty())
					value = data->FormatProperty(prop);
				if (!value.empty()) {
					if (value.size() > 102)
						value = value.substr(0, 100) + L"...";
					details += prop.Name + L": " + value + L";";
				}
			}
		}
	}
	return details;
}

void CEtwView::UpdateEventStatus() {
	CString text;
	text.Format(L"Events: %u", (uint32_t)m_Events.size());
	GetFrame()->SetPaneText(2, text);
}

bool CEtwView::IsSortable(HWND, int col) const {
	return col != 8 && (!m_IsMonitoring || GetFrame()->GetTraceManager().IsPaused());
}

void CEtwView::DoSort(const SortInfo* si) {
	auto compare = [&](auto& i1, auto& i2) {
		switch (si->SortColumn)
		{
			case 0: return SortHelper::SortNumbers(i1->GetIndex(), i2->GetIndex(), si->SortAscending);
			case 1: return SortHelper::SortNumbers(i1->GetTimeStamp(), i2->GetTimeStamp(), si->SortAscending);
			case 2: return SortHelper::SortStrings(i1->GetEventName(), i2->GetEventName(), si->SortAscending);
			case 3: return SortHelper::SortNumbers(i1->GetProcessId(), i2->GetProcessId(), si->SortAscending);
			case 4: return SortHelper::SortStrings(i1->GetProcessName(), i2->GetProcessName(), si->SortAscending);
			case 5: return SortHelper::SortNumbers(i1->GetThreadId(), i2->GetThreadId(), si->SortAscending);
			case 6: return SortHelper::SortNumbers(i1->GetEventDescriptor().Opcode, i2->GetEventDescriptor().Opcode, si->SortAscending);
		}
		return false;
	};

	if (m_Events.size() < 20000)
		std::sort(m_Events.begin(), m_Events.end(), compare);
	else {
		// C++ 17引入了执行策略
		// std::execution::par使算法在多个线程中执行，并且线程各自具有自己的顺序任务。即并行但不并发。
		// 并行在操作系统中是指，一组程序按独立异步的速度执行，不等于时间上的重叠（同一个时刻发生）。
		// 并发是指：在同一个时间段内，两个或多个程序执行，有时间上的重叠（宏观上是同时，微观上仍是顺序执行）。
		std::sort(std::execution::par, m_Events.begin(), m_Events.end(), compare);
	}
}

BOOL CEtwView::PreTransalteMessage(MSG* pMsg) {
	pMsg;
	return FALSE;
}

DWORD CEtwView::OnPrePaint(int, LPNMCUSTOMDRAW) {
	return CDRF_NOTIFYITEMDRAW;
}

DWORD CEtwView::OnSubItemPrePaint(int, LPNMCUSTOMDRAW cd) {
	auto lcd = (LPNMLVCUSTOMDRAW)cd;
	auto cm = GetColumnManager(m_List);
	auto sub = cm->GetRealColumn(lcd->iSubItem);
	lcd->clrTextBk = CLR_INVALID;

	if ((cm->GetColumn(sub).Flags & ColumnFlags::Numeric) == ColumnFlags::Numeric)
		::SelectObject(cd->hdc, (HFONT)GetFrame()->GetMonoFont());
	else
		::SelectObject(cd->hdc, m_hFont);

	int index = (int)cd->dwItemSpec;
	if (sub == 8 && (m_List.GetItemState(index, LVIS_SELECTED) & LVIS_SELECTED) == 0) {
		auto& item = m_Events[index];
		auto start = (size_t)0;
		auto details = GetEventDetails(item.get());
		bool bold = false;
		CDCHandle dc(cd->hdc);
		std::wstring str;
		int x = cd->rc.left + 6, y = cd->rc.top, right = cd->rc.right;
		SIZE size;
		for (;;) {
			auto pos = details.find(L";", start);
			if (pos == std::wstring::npos)
				break;
			str = details.substr(start, pos - start);

			auto colon = str.find(L":");
			dc.SetTextColor(::GetSysColor(COLOR_WINDOWTEXT));
			dc.GetTextExtent(str.c_str(), (int)colon + 1, &size);
			if (x + size.cx > right)
				break;

			dc.TextOutW(x, y, str.c_str(), (int)colon + 1);
			x += size.cx;

			dc.SetTextColor(RGB(0, 0, 255));
			dc.GetTextExtent(str.data() + colon + 1, (int)str.size() - (int)colon - 1, &size);
			if (x + size.cx > right)
				break;
			dc.TextOutW(x, y, str.data() + colon + 1, (int)str.size() - (int)colon - 1);
			x += size.cx + 4;

			start = pos + 1;
		}
		return CDRF_SKIPDEFAULT;
	}

	return CDRF_NEWFONT | CDRF_NOTIFYSUBITEMDRAW;
}

DWORD CEtwView::OnItemPrePaint(int, LPNMCUSTOMDRAW cd) {
	m_hFont = (HFONT)::GetCurrentObject(cd->hdc, OBJ_FONT);
	return CDRF_NOTIFYSUBITEMDRAW;
}

CImageList CEtwView::GetEventImageList() {
	return s_Images;
}

int CEtwView::GetImageFromEventName(PCWSTR name) {
	if (auto it = s_IconsMap.find(name); it != s_IconsMap.end())
		return it->second;

	auto pos = ::wcschr(name, L'/');
	if (pos) {
		if (auto it = s_IconsMap.find(std::wstring(name, pos)); it != s_IconsMap.end())
			return it->second;
	}
	return 0;
}

void CEtwView::OnFinalMessage(HWND) {
	GetFrame()->ViewDestroyed(this);
	delete this;
}

LRESULT CEtwView::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
	m_hWndClient = m_List.Create(*this, rcDefault, nullptr,
		WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
		LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHAREIMAGELISTS);
	m_List.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP |
		LVS_EX_DOUBLEBUFFER);

	auto processes = KernelEventCategory::GetCategory(L"Process");
	ATLASSERT(processes);

	// init event
	EventConfigCategory cat;
	cat.Name = processes->Name;
	m_EventsConfig.AddCategory(cat);

	// init filters
	FilterDescription desc;
	desc.Name = L"Process Id";
	desc.Action = FilterAction::Exclude;
	desc.Parameters = std::to_wstring(::GetCurrentProcessId());
	m_FilterConfig.AddFilter(desc);

	if (s_Images == nullptr) {
		s_Images.Create(16, 16, ILC_COLOR32, 8, 8);
		struct {
			int icon;
			PCWSTR type;
		}icons[] = {
			{IDI_GENERIC,nullptr},
			{ IDI_HEAP2, L"PageFault/VirtualAlloc" },
			{ IDI_HEAP2, L"Virtual Memory" },
			{ IDI_GEAR, L"Process" },
			{ IDI_PROCESS_NEW, L"Process/Start" },
			{ IDI_PROCESS_DELETE, L"Process/Terminate" },
			{ IDI_PROCESS_DELETE, L"Process/End" },
			{ IDI_THREAD, L"Thread" },
			{ IDI_THREAD_NEW, L"Thread/Start" },
			{ IDI_THREAD_DELETE, L"Thread/End" },
			{ IDI_DLL, L"Image" },
			{ IDI_DLL_LOAD, L"Image/Load" },
			{ IDI_DLL_UNLOAD, L"Image/UnLoad" },
			{ IDI_NETWORK, L"TCP" },
			{ IDI_NETWORK, L"UDP" },
			{ IDI_NETWORK, L"TcpIp" },
			{ IDI_NETWORK, L"UdpIp" },
			{ IDI_REGISTRY, L"Registry" },
			{ IDI_FILE, L"FileIo" },
			{ IDI_FILE, L"File" },
			{ IDI_HANDLE, L"Objects" },
			{ IDI_HANDLE, L"Object" },
			{ IDI_OBJECT, L"Object/CreateHandle" },
			{ IDI_OBJECT, L"Object/CloseHandle" },
			{ IDI_OBJECT, L"Handles" },
			{ IDI_DISK, L"DiskIo" },
			{ IDI_DISK, L"Disk I/O" },
			{ IDI_MEMORY, L"PageFault" },
			{ IDI_MEMORY, L"Page Fault" },
			{ IDI_HEAP, L"Pool" },
			{ IDI_HEAP, L"Kernel Pool" },
		};

		int index = 0;
		for (auto entry : icons) {
			s_Images.AddIcon(AtlLoadIconImage(entry.icon, 0, 16, 16));
			if (entry.type)
				s_IconsMap.insert({ entry.type,index });
			index++;
		}
	}

	m_List.SetImageList(s_Images, LVSIL_SMALL);

	auto cm = GetColumnManager(m_List);
	cm->AddColumn(L"#", 0, 100, ColumnFlags::Numeric | ColumnFlags::Visible);
	cm->AddColumn(L"Time", LVCFMT_RIGHT, 180, ColumnFlags::Numeric | ColumnFlags::Visible);
	cm->AddColumn(L"Event", LVCFMT_LEFT, 160);
	cm->AddColumn(L"PID", LVCFMT_RIGHT, 120, ColumnFlags::Numeric | ColumnFlags::Visible);
	cm->AddColumn(L"Process Name", LVCFMT_LEFT, 150);
	cm->AddColumn(L"TID", LVCFMT_RIGHT, 120, ColumnFlags::Numeric | ColumnFlags::Visible);
	cm->AddColumn(L"Details", LVCFMT_LEFT, 700);

	cm->UpdateColumns();

	m_TempEvents.reserve(1 << 12);
	m_Events.reserve(1 << 16);
	SetTimer(1, 1000, nullptr);

	return 0;
}

LRESULT CEtwView::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
	KillTimer(1);
	handled = FALSE;
	return 0;
}

LRESULT CEtwView::OnForwardMsg(UINT, WPARAM, LPARAM lp, BOOL& handled) {
	auto msg = (MSG*)lp;
	LRESULT result;
	handled = ProcessWindowMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam, result, 1);
	return result;
}

LRESULT CEtwView::OnTimer(UINT, WPARAM id, LPARAM, BOOL&) {
	if (m_IsMonitoring && id == 1) {
		if (!m_TempEvents.empty()) {
			std::lock_guard lock(m_EventsLock);
			m_Events.insert(m_Events.end(), m_TempEvents.begin(), m_TempEvents.end());
			m_OrgEvents.insert(m_OrgEvents.end(), m_TempEvents.begin(), m_TempEvents.end());
			m_TempEvents.clear();
		}
		else if (!m_IsMonitoring) {
			m_IsDraining = false;
		}
		UpdateList(m_List, static_cast<int>(m_Events.size()));
		if (m_AutoScroll)
			m_List.EnsureVisible(m_List.GetItemCount() - 1, FALSE);
		if (m_IsActive)
			UpdateEventStatus();
	}
	return 0;
}

LRESULT CEtwView::OnClear(WORD, WORD, HWND, BOOL&) {
	CWaitCursor wait;
	m_List.SetItemCount(0);
	std::lock_guard lock(m_EventsLock);
	m_Events.clear();
	m_TempEvents.clear();
	m_OrgEvents.clear();
	return 0;
}

LRESULT CEtwView::OnCallStack(WORD, WORD, HWND, BOOL&) {
	auto selected = m_List.GetSelectedIndex();
	if (selected < 0)
		return 0;

	auto data = m_Events[selected].get();
	if (data->GetStackEventData() == nullptr) {
		AtlMessageBox(*this, L"Call stack not available for this event", IDS_TITLE, MB_ICONEXCLAMATION);
		return 0;
	}

	CCallStackDlg dlg(data);
	dlg.DoModal();

	return 0;
}

LRESULT CEtwView::OnEventProperties(WORD, WORD, HWND, BOOL&) {
	auto selected = m_List.GetSelectedIndex();
	if (selected < 0)
		return 0;

	auto data = m_Events[selected].get();
	CEventPropertiesDlg dlg(data);
	dlg.DoModal();

	return 0;
}

LRESULT CEtwView::OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
	if (m_IsMonitoring) {
		AtlMessageBox(nullptr, L"Cannot close tab while monitoring is active", IDS_TITLE, MB_ICONWARNING);
		handled = TRUE;
	}
	handled = FALSE;
	return 0;
}

LRESULT CEtwView::OnCopy(WORD, WORD, HWND, BOOL&) {
	auto selected = m_List.GetSelectedIndex();
	ATLASSERT(selected >= 0);
	CString text, item;
	for (int c = 0;; c++) {
		if (!m_List.GetItemText(selected, c, item))
			break;
		text += item + L",";
	}
	ClipboardHelper::CopyText(*this, text.Left(text.GetLength() - 1));

	return 0;
}

LRESULT CEtwView::OnSave(WORD, WORD, HWND, BOOL&) {
	CSimpleFileDialog dlg(FALSE, L"pmx", nullptr, OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_ENABLESIZING,
		L"ProcMonX files (*.pmx)\0*.pmx\0CSV Files (*csv)\0*.csv\0", *this);
	if (dlg.DoModal() == IDOK) {
		CString filename(dlg.m_szFileTitle);
		auto ext = filename.ReverseFind(L'.');
		if (ext > 0) {
			auto serializer = SerializerFactory::CreateFromExtension(filename.Mid(ext + 1));
			if (serializer) {
				EventDataSerializerOptions options;
				CWaitCursor wait;
				auto eventsCopy = m_OrgEvents;
				if (!serializer->Save(eventsCopy, options, dlg.m_szFileName))
					AtlMessageBox(*this, L"Failed to save data", IDS_TITLE, MB_ICONERROR);
				return 0;
			}
		}
		AtlMessageBox(*this, L"Unknown file extension", IDS_TITLE, MB_ICONEXCLAMATION);
	}
	return 0;
}

LRESULT CEtwView::OnCopyAll(WORD, WORD, HWND, BOOL&) {
	ATLASSERT(!m_IsMonitoring || GetFrame()->GetTraceManager().IsPaused());
	CWaitCursor wait;
	auto count = m_List.GetItemCount();
	CString text, item;
	for (int i = 0; i < count; i++) {
		for (int c = 0;; c++) {
			if (!m_List.GetItemText(i, c, item))
				break;
			text += item + L",";
		}
		text.SetAt(text.GetLength() - 1, L'\n');
	}
	ClipboardHelper::CopyText(*this, text);
	return 0;
}

LRESULT CEtwView::OnConfigureEvents(WORD, WORD, HWND, BOOL&) {
	CEventsDlg dlg(m_EventsConfig);
	if (dlg.DoModal() == IDOK) {
		if (m_IsMonitoring) {
			// update trace manager
			auto& tm = GetFrame()->GetTraceManager();
			std::vector<KernelEventTypes> types;
			std::vector<std::wstring> categories;
			DWORD flags = 0;
			for (auto& cat : m_EventsConfig.GetCategories()) {
				auto c = KernelEventCategory::GetCategory(cat.Name.c_str());
				ATLASSERT(c);
				types.push_back(c->EnableFlag);
				categories.push_back(c->Name);
				flags |= (DWORD)c->EnableFlag;
			}
			std::initializer_list<KernelEventTypes> events(types.data(), types.data() + types.size());
			tm.SetKernelEventTypes(events);
			tm.SetKernelEventStacks(std::initializer_list<std::wstring>(categories.data(),categories.data() + categories.size()));
			bool isWin8Plus = ::IsWindows8OrGreater();

			if (isWin8Plus) {
				tm.UpdateEventConfig();
			}
			else {
				tm.Stop();
				tm.Pause(false);
				tm.Start([&](auto data) {
					this->AddEvent(data);
					}, flags);
			}
		}
	}
	return 0;
}

LRESULT CEtwView::OnAutoScroll(WORD, WORD, HWND, BOOL&) {
	m_AutoScroll = !m_AutoScroll;
	GetFrame()->GetUpdateUI()->UISetCheck(ID_VIEW_AUTOSCROLL, m_AutoScroll);

	return 0;
}

LRESULT CEtwView::OnConfigFilters(WORD, WORD, HWND, BOOL&) {
	CFiltersDlg dlg(m_FilterConfig);
	if (dlg.DoModal() == IDOK) {
		// update filters
		auto& tm = GetFrame()->GetTraceManager();
		auto paused = tm.IsRunning() && tm.IsPaused();
		if (!paused)
			tm.Pause(true);
		ApplyFilters(m_FilterConfig);
		if (!paused)
			tm.Pause(false);
	}
	return 0;
}

LRESULT CEtwView::OnItemChanged(int, LPNMHDR, BOOL&) {
	UpdateUI();

	return 0;
}

LRESULT CEtwView::OnFindNext(WORD, WORD, HWND, BOOL&) {
	auto& options = CQuickFindDlg::GetSearchOptions();
	auto& text = CQuickFindDlg::GetSearchText();

	auto step = options.SearchDown ? 1 : -1;
	auto count = (int)m_Events.size();
	if (count == 0)
		return 0;

	auto selected = m_List.GetSelectedIndex() + step;
	if (selected < 0)
		selected = count - 1;
	else if (selected >= count)
		selected = 0;

	auto start = selected;
	auto search = text;
	auto cs = options.CaseSensitive;
	if (!cs)
		search.MakeLower();

	// perfrom search
	bool found = false;
	do {
		auto& evt = *m_Events[selected];
		if (options.SearchProcesses) {
			CString text = evt.GetProcessName().c_str();
			if (!cs)
				text.MakeLower();
			if (text.Find(search) >= 0) {
				found = true;
				break;
			}
		}
		if (options.SearchEvents) {
			CString text = evt.GetEventName().c_str();
			if (!cs)
				text.MakeLower();
			if (text.Find(search) >= 0) {
				found = true;
				break;
			}
		}
		if (options.SearchDetails) {
			CString text = GetEventDetails(&evt).c_str();
			if (!cs)
				text.MakeLower();
			if (text.Find(search) >= 0) {
				found = true;
				break;
			}
		}

		// move to the next row
		selected += step;
		if (selected >= count)
			selected = 0;
		else if (selected < 0)
			selected = count - 1;
	} while (selected != start);

	if (found) {
		m_List.SelectItem(selected);
		m_List.SetFocus();
	}
	else
		AtlMessageBox(*this, L"Text not found", IDS_TITLE, MB_ICONINFORMATION);

	return 0;
}

void CEtwView::UpdateUI() {
	auto ui = GetFrame()->GetUpdateUI();
	auto& tm = GetFrame()->GetTraceManager();

	ui->UIEnable(ID_EDIT_COPYALL, !m_IsMonitoring || tm.IsPaused());
	ui->UIEnable(ID_FILE_SAVE, !m_IsMonitoring || tm.IsPaused());
	ui->UISetCheck(ID_VIEW_AUTOSCROLL, m_AutoScroll);
	auto selected = m_List.GetSelectedIndex();
	ui->UIEnable(ID_EVENT_PROPERTIES, selected >= 0);
	ui->UIEnable(ID_EDIT_COPY, selected >= 0);
	ui->UIEnable(ID_EVENT_CALLSTACK, selected >= 0 && m_Events[selected]->GetStackEventData() != nullptr);
}

void CEtwView::ApplyFilters(const FilterConfiguration& config) {
	auto& tm = GetFrame()->GetTraceManager();
	tm.RemoveAllFilters();
	for (int i = 0; i < config.GetFilterCount(); i++) {
		auto desc = config.GetFilter(i);
		ATLASSERT(desc);
		auto filter = FilterFactory::CreateFilter(desc->Name.c_str(), desc->Compare, 
			desc->Parameters.c_str(),desc->Action);
		ATLASSERT(filter);
		if (filter) {
			filter->Enable(desc->Enabled);
			tm.AddFilter(filter);
		}
	}
}