#pragma once
#include "SSDTHookTable.h"


class CKernelHookView:
	public CWindowImpl<CKernelHookView>{
public:
	DECLARE_WND_CLASS(nullptr);

	const UINT TabId = 1235;
	CKernelHookView() :m_TabCtrl(this) {
	}

	BEGIN_MSG_MAP(CKernelHookView)
		MESSAGE_HANDLER(WM_CREATE, OnCreate)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		NOTIFY_HANDLER(TabId,TCN_SELCHANGE,OnTcnSelChange)
	END_MSG_MAP()

	LRESULT OnCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnSize(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnTcnSelChange(int, LPNMHDR hdr, BOOL&);

	void InitSSDTHookTable();

	enum class TabColumn : int {
		SSDT,ShadowSSDT,ObjectCallback
	};
private:
	// ��̬���������Ŀؼ�
	CContainedWindowT<CTabCtrl> m_TabCtrl;

	CSSDTHookTable* m_SSDTHookTable;
	HWND m_hwndArray[16];
	int _index = 0;
};