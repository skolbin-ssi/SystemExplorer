#pragma once

#include "Interfaces.h"
#include "resource.h"

template<typename T>
struct PauseResumeUpdates {
	PauseResumeUpdates(T* view) : _view(view), _paused(view->IsPaused()) {
		if (!_paused)
			_view->Pause(true);
	}
	~PauseResumeUpdates() {
		if (!_paused)
			_view->Pause(false);
	}

	T* _view;
	bool _paused;
};

template<typename T, typename TBase = CFrameWindowImpl<T, CWindow, CControlWinTraits>>
class CViewBase abstract : 
	public TBase,
	public IView,
	public CAutoUpdateUI<T>,
	public CIdleHandler {
public:
	CViewBase(IMainFrame* frame) : m_pFrame(frame) {
		ATLASSERT(frame);
	}

	int GetUpdateInterval() const {
		return m_UpdateInterval;
	}

	bool IsPaused() const {
		return m_Paused;
	}

protected:
	BEGIN_MSG_MAP(CViewBase)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		//MESSAGE_HANDLER(WM_FORWARDMSG, OnForwardMessage)
		COMMAND_ID_HANDLER(ID_VIEW_REFRESH, OnRefresh)
		COMMAND_ID_HANDLER(ID_VIEW_PAUSE, OnPauseResume)
		MESSAGE_HANDLER(OM_ACTIVATE_PAGE, OnActivate)
		COMMAND_RANGE_HANDLER(ID_UPDATEINTERVAL_1SECOND, ID_UPDATEINTERVAL_10SECONDS, OnUpdateInterval)
		MESSAGE_HANDLER(OM_NEW_FRAME, OnNewFrame)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		CHAIN_MSG_MAP(TBase)
	END_MSG_MAP()

	LRESULT OnRefresh(WORD, WORD, HWND, BOOL&) {
		auto pT = static_cast<T*>(this);
		pT->DoRefresh();
		return 0;
	}

	LRESULT OnActivate(UINT /*uMsg*/, WPARAM activate, LPARAM, BOOL&) {
		auto pT = static_cast<T*>(this);
		auto ui = Frame()->GetUpdateUI();
		if (activate) {
			ui->UIEnable(ID_EDIT_FIND, pT->IsFindSupported());
			ui->UIEnable(ID_EDIT_FIND_NEXT, pT->IsFindSupported());
		}

		if (pT->IsUpdating()) {
			if (activate) {
				ui->UISetRadioMenuItem(m_CurrentUpdateId, ID_UPDATEINTERVAL_1SECOND, ID_UPDATEINTERVAL_10SECONDS);
				ui->UISetCheck(ID_VIEW_PAUSE, m_Paused);
				ui->UIEnable(ID_VIEW_PAUSE, TRUE);

				pT->SetTimer(1, GetUpdateInterval(), nullptr);
			}
			else {
				pT->KillTimer(1);
			}
		}
		else if(activate) {
			ui->UIEnable(ID_VIEW_PAUSE, FALSE);
		}
		pT->OnActivate(activate);
		return 0;
	}

	LRESULT OnForwardMessage(UINT /*uMsg*/, WPARAM id, LPARAM lParam, BOOL& handled) {
		auto msg = reinterpret_cast<MSG*>(lParam);
		LRESULT result = 0;
		handled = static_cast<T*>(this)->ProcessWindowMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam, result, 1);
		return result;
	}

	LRESULT OnTimer(UINT /*uMsg*/, WPARAM id, LPARAM lParam, BOOL& bHandled) {
		if (id != 1) {
			bHandled = FALSE;
			return 0;
		}
		static_cast<T*>(this)->OnUpdate();
		return 0;
	}

	LRESULT OnPauseResume(WORD, WORD, HWND, BOOL&) {
		Pause(!m_Paused);
		static_cast<T*>(this)->OnPauseResume(m_Paused);
		return 0;
	}

	void Pause(bool pause) {
		m_Paused = pause;
		auto pT = static_cast<T*>(this);
		if (m_Paused)
			pT->KillTimer(1);
		else
			pT->SetTimer(1, GetUpdateInterval(), nullptr);
	}

	LRESULT OnUpdateInterval(WORD, WORD id, HWND, BOOL&) {
		int intervals[] = { 1000, 2000, 5000, 10000 };
		int index = id - ID_UPDATEINTERVAL_1SECOND;
		ATLASSERT(index >= 0 && index < _countof(intervals));

		m_UpdateInterval = intervals[index];
		auto pT = static_cast<T*>(this);
		if (!m_Paused) {
			pT->SetTimer(1, m_UpdateInterval, nullptr);
		}
		pT->OnUpdateIntervalChanged(m_UpdateInterval);
		Frame()->GetUpdateUI()->UISetRadioMenuItem(m_CurrentUpdateId = id, ID_UPDATEINTERVAL_1SECOND, ID_UPDATEINTERVAL_10SECONDS);
		return 0;
	}

	void OnPauseResume(bool paused) {}
	void OnUpdateIntervalChanged(int interval) {}
	void OnUpdate() {}
	void OnActivate(bool) {}
	void DoRefresh() {}

	bool IsUpdating() const {
		return true;
	}

	BOOL OnIdle() override {
		CAutoUpdateUI<T>::UIUpdateToolBar();
		return FALSE;
	}

	IMainFrame* Frame() const {
		return m_pFrame;
	}

	LRESULT OnNewFrame(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
		m_pFrame = reinterpret_cast<IMainFrame*>(lParam);
		return 0;
	}

	LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
		auto pT = static_cast<T*>(this);

		bHandled = FALSE;
		if(pT->m_hWndToolBar)
			_Module.GetMessageLoop()->RemoveIdleHandler(this);
		return 0;
	}

	struct ToolBarButtonInfo {
		UINT id;
		int image;
		BYTE style = BTNS_BUTTON;
		PCWSTR text = nullptr;
	};

	HWND CreateAndInitToolBar(const ToolBarButtonInfo* buttons, int count) {
		auto pT = static_cast<T*>(this);

		CToolBarCtrl tb;
		auto hWndToolBar = tb.Create(pT->m_hWnd, pT->rcDefault, nullptr, ATL_SIMPLE_TOOLBAR_PANE_STYLE | TBSTYLE_LIST, 0, ATL_IDW_TOOLBAR);
		tb.SetExtendedStyle(TBSTYLE_EX_MIXEDBUTTONS);

		CImageList tbImages;
		tbImages.Create(24, 24, ILC_COLOR32, 4, 4);
		tb.SetImageList(tbImages);

		for (int i = 0; i < count; i++) {
			auto& b = buttons[i];
			if (b.id == 0)
				tb.AddSeparator(0);
			else {
				int image = b.image == 0 ? I_IMAGENONE : tbImages.AddIcon(AtlLoadIconImage(b.image, 0, 24, 24));
				tb.AddButton(b.id, b.style | (b.text ? BTNS_SHOWTEXT : 0), TBSTATE_ENABLED, image, b.text, 0);
			}
		}
		pT->CreateSimpleReBar(ATL_SIMPLE_REBAR_NOBORDER_STYLE);
		pT->AddSimpleReBarBand(tb);

		pT->UIAddToolBar(hWndToolBar);
		_Module.GetMessageLoop()->AddIdleHandler(this);

		return hWndToolBar;
	}

private:
	IMainFrame* m_pFrame;
	int m_CurrentUpdateId = ID_UPDATEINTERVAL_1SECOND;
	int m_UpdateInterval{ 1000 };
	bool m_Paused{ false };
};
