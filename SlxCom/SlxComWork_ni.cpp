#include "SlxComWork_ni.h"
#include "SlxComTools.h"
#include "resource.h"
#include <Shlwapi.h>
#pragma warning(disable: 4786)
#include <vector>
#include <map>
#include <set>
#include "lib/charconv.h"

using namespace std;

#define NOTIFYWNDCLASS  TEXT("__slxcom_work_ni_notify_wnd")
#define WM_CALLBACK     (WM_USER + 112)
#define ICON_COUNT      10
#define TIMER_ICON      1
#define TIMER_MENU      2

extern HBITMAP g_hKillExplorerBmp; //SlxCom.cpp

static HHOOK g_hMsgHook = NULL;
static BOOL g_bChangeButtonText = FALSE;

static enum
{
    SYS_QUIT        = 1,
    SYS_ABOUT,
    SYS_RESETEXPLORER,
    SYS_WINDOWMANAGER,
    SYS_UPDATEMENU,
    SYS_MAXVALUE,
};

class MenuItem
{
public:
    MenuItem() : m_nCmdId(0) { }
    virtual ~MenuItem() { }
    virtual void DoJob(HWND hWindow, HINSTANCE hInstance) = 0;
    virtual bool operator<(const MenuItem &other) const { return m_nCmdId < other.m_nCmdId;}

protected:
    int m_nCmdId;
    tstring m_strText;
};

class MenuItemRegistry : public MenuItem
{
public:
    MenuItemRegistry(int nCmdId, LPCTSTR lpText, LPCTSTR lpRegPath)
    {
        m_nCmdId = nCmdId;
        AssignString(m_strText, lpText);
        AssignString(m_strRegPath, lpRegPath);
    }

    virtual void DoJob(HWND hWindow, HINSTANCE hInstance)
    {
        LPCTSTR lpRegPath = NULL;
        HKEY hRootKey = ParseRegPath(m_strRegPath.c_str(), &lpRegPath);

        if (hRootKey == NULL)
        {
            MessageBoxFormat(hWindow, NULL, MB_ICONERROR, TEXT("%s不是合法的注册表路径，无法自动导航。"), m_strRegPath.c_str());
        }
        else
        {
            if (IsRegPathExists(hRootKey, lpRegPath))
            {
                BrowseForRegPath(m_strRegPath.c_str());
            }
            else
            {
                g_bChangeButtonText = TRUE;

                int nId = MessageBoxFormat(
                    hWindow,
                    TEXT("请确认"),
                    MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON3,
                    TEXT("%s路径不存在，您可以跳转到最接近的位置或创建这个路径。\r\n\r\n")
                    TEXT("选择“就近”跳转到最接近的位置\r\n")
                    TEXT("选择“创建”自动创建此路径并跳转\r\n"),
                    m_strRegPath.c_str()
                    );

                g_bChangeButtonText = FALSE;

                if (nId == IDYES)       //就近
                {
                    int len = (lstrlen(lpRegPath) + 1);
                    TCHAR *szPath = (TCHAR *)malloc(len * sizeof(TCHAR));

                    if (szPath != NULL)
                    {
                        lstrcpyn(szPath, lpRegPath, len);
                        PathRemoveBackslash(szPath);

                        while (!IsRegPathExists(hRootKey, szPath))
                        {
                            PathRemoveFileSpec(szPath);
                        }

                        tstring::size_type pos = m_strRegPath.find(TEXT("\\"));

                        if (pos != m_strRegPath.npos)
                        {
                            BrowseForRegPath((m_strRegPath.substr(0, pos + 1) + szPath).c_str());
                        }

                        free((void *)szPath);
                    }
                }
                else if (nId == IDNO)   //创建
                {
                    if (TouchRegPath(hRootKey, lpRegPath))
                    {
                        BrowseForRegPath(m_strRegPath.c_str());
                    }
                    else
                    {
                        MessageBoxFormat(hWindow, NULL, MB_ICONERROR, TEXT("创建%s失败。"), m_strRegPath.c_str());
                    }
                }
            }
        }
    }

protected:
    tstring m_strRegPath;
};

class MenuMgr
{
public:
    MenuMgr(HWND hWindow, HINSTANCE hInstance)
        : m_hWindow(hWindow), m_hInstance(hInstance)
    {
        m_hMenuFont = GetMenuDefaultFont();
        m_hMenuDc = CreateCompatibleDC(NULL);
        m_hOldFont = SelectObject(m_hMenuDc, (HGDIOBJ)m_hMenuFont);

        m_hMenu = NULL;
        UpdateMenu();
    }

    ~MenuMgr()
    {
        SelectObject(m_hMenuDc, m_hOldFont);
        DeleteDC(m_hMenuDc);

        if (m_hMenuFont)
        {
            DeleteObject(m_hMenuFont);
        }
    }

    void TrackAndDealPopupMenu(int x, int y)
    {
        int nCmd = TrackPopupMenu(
            m_hMenu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
            x,
            y,
            NULL,
            m_hWindow,
            NULL
            );

        if (nCmd == 0)
        {
            return;
        }

        switch (nCmd)
        {
        case SYS_QUIT:
            PostMessage(m_hWindow, WM_SYSCOMMAND, SC_CLOSE, 0);
            break;

        case SYS_ABOUT:
            MessageBox(m_hWindow, TEXT("About"), NULL, 0);
            break;

        case SYS_RESETEXPLORER:
            if (IDYES == MessageBox(
                m_hWindow,
                TEXT("要重启Explorer（操作系统外壳程序）吗？"),
                TEXT("请确认"),
                MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON3 | MB_SYSTEMMODAL
                ))
            {
                KillAllExplorers();
            }
            break;

        case SYS_WINDOWMANAGER:
            MessageBox(m_hWindow, NULL, NULL, MB_SYSTEMMODAL);
            break;

        case SYS_UPDATEMENU:
            UpdateMenu();
            break;

        default:
            if (m_mapMenuItems.find(nCmd) != m_mapMenuItems.end())
            {
                m_mapMenuItems[nCmd]->DoJob(m_hWindow, m_hInstance);
            }
            break;
        }
    }

    void UpdateMenu()
    {
        m_setMenuItemsRegistry.clear();
        m_mapMenuItems.clear();

        if (m_hMenu != NULL)
        {
            DestroyMenu(m_hMenu);
        }

        UINT nMenuId = SYS_MAXVALUE;
        m_hMenu = CreatePopupMenu();

        //主菜单
        AppendMenu(m_hMenu, MF_STRING | MF_DEFAULT, SYS_WINDOWMANAGER, TEXT("窗口管理器(&W)..."));
        AppendMenu(m_hMenu, MF_POPUP, (UINT)InitRegPathSubMenu(&nMenuId), TEXT("注册表位置(&R)"));
        AppendMenu(m_hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(m_hMenu, MF_STRING, SYS_RESETEXPLORER, TEXT("重新启动Explorer(&R)"));
        AppendMenu(m_hMenu, MF_SEPARATOR, 0, NULL);
        SetMenuItemBitmaps(m_hMenu, SYS_RESETEXPLORER, MF_BYCOMMAND, g_hKillExplorerBmp, g_hKillExplorerBmp);
        AppendMenu(m_hMenu, MF_STRING, SYS_UPDATEMENU, TEXT("刷新菜单内容(&U)"));
      //AppendMenu(m_hMenu, MF_STRING, SYS_ABOUT, TEXT("关于(&A)..."));
        AppendMenu(m_hMenu, MF_STRING, SYS_QUIT, TEXT("不显示托盘图标(&Q)"));
    }

private:
    //获取指定注册表路径下的所有子项列表，子项列表中不含路径部分
    static set<tstring> GetRegAllSubPath(HKEY hRootKey, LPCTSTR lpRegPath)
    {
        set<tstring> result;
        HKEY hKey = NULL;

        if (ERROR_SUCCESS == RegOpenKeyEx(hRootKey, lpRegPath, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hKey))
        {
            DWORD dwSubKeyCount = 0;
            DWORD dwMaxSubKeyLen = 0;

            if (ERROR_SUCCESS == RegQueryInfoKey(
                hKey,
                NULL,
                NULL,
                NULL,
                &dwSubKeyCount,
                &dwMaxSubKeyLen,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL
                ))
            {
                dwMaxSubKeyLen += 1;
                dwMaxSubKeyLen *= 2;

                TCHAR *szSubKey = (TCHAR *)malloc(dwMaxSubKeyLen);

                if (szSubKey != NULL)
                {
                    for (DWORD dwIndex = 0; dwIndex < dwSubKeyCount; dwIndex += 1)
                    {
                        DWORD dwSubKeySize = dwMaxSubKeyLen;

                        if (ERROR_SUCCESS == RegEnumKeyEx(hKey, dwIndex, szSubKey, &dwSubKeySize, NULL, NULL, NULL, NULL))
                        {
                            result.insert(szSubKey);
                        }
                    }
                }

                if (szSubKey != NULL)
                {
                    free((void *)szSubKey);
                }
            }

            RegCloseKey(hKey);
        }

        return result;
    }

    //获取指定注册表路径下的所有字符串键值对应表，只关注REG_SZ类型
    static map<tstring, tstring> GetRegAllSzValuePairs(HKEY hRootKey, LPCTSTR lpRegPath)
    {
        map<tstring, tstring> result;
        HKEY hKey = NULL;

        if (ERROR_SUCCESS == RegOpenKeyEx(hRootKey, lpRegPath, 0, KEY_QUERY_VALUE, &hKey))
        {
            DWORD dwValueCount = 0;
            DWORD dwMaxValueNameLen = 0;
            DWORD dwMaxDataLen = 0;

            if (ERROR_SUCCESS == RegQueryInfoKey(
                hKey,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                &dwValueCount,
                &dwMaxValueNameLen,
                &dwMaxDataLen,
                NULL,
                NULL
                ))
            {
                dwMaxValueNameLen += 1;
                dwMaxValueNameLen *= 2;
                dwMaxDataLen += 1;

                TCHAR *szValueName = (TCHAR *)malloc(dwMaxValueNameLen);
                unsigned char *szData = (unsigned char *)malloc(dwMaxDataLen);

                if (szValueName != NULL && szData != NULL)
                {
                    for (DWORD dwIndex = 0; dwIndex < dwValueCount; dwIndex += 1)
                    {
                        DWORD dwRegType = REG_NONE;
                        DWORD dwValueSize = dwMaxValueNameLen;
                        DWORD dwDataSize = dwMaxDataLen;

                        if (ERROR_SUCCESS == RegEnumValue(
                            hKey,
                            dwIndex,
                            szValueName,
                            &dwValueSize,
                            NULL,
                            &dwRegType,
                            szData,
                            &dwDataSize
                            ))
                        {
                            if (dwRegType == REG_SZ)
                            {
                                result[szValueName] = (LPCTSTR)szData;
                            }
                        }
                    }
                }

                if (szValueName != NULL)
                {
                    free((void *)szValueName);
                }

                if (szData != NULL)
                {
                    free((void *)szData);
                }
            }

            RegCloseKey(hKey);
        }

        return result;
    }

    //将指定注册表路径映射为菜单，菜单项ID使用pMenuId指向的值递增使用
    //bEraseHead如被指定，则删除值中的最前的一个路径段
    //bHaveChildPath如被指定，则处理子项
    HMENU RegistryPathToMenu(UINT *pMenuId, HKEY hRootKey, LPCTSTR lpRegPath, BOOL bEraseHead, BOOL bHaveChildPath)
    {
        HMENU hPopupMenu = CreatePopupMenu();

        //处理子项
        if (bHaveChildPath)
        {
            set<tstring> setSubPaths = GetRegAllSubPath(hRootKey, lpRegPath);
            set<tstring>::iterator itSubPath = setSubPaths.begin();
            for (; itSubPath != setSubPaths.end(); ++itSubPath)
            {
                tstring strSubPath = lpRegPath;

                strSubPath += TEXT("\\");
                strSubPath += *itSubPath;

                AppendMenu(
                    hPopupMenu,
                    MF_POPUP,
                    (UINT)RegistryPathToMenu(pMenuId, hRootKey, strSubPath.c_str(), FALSE, TRUE),
                    itSubPath->c_str()
                    );
            }
        }

        //处理当前项下的键
        map<tstring, tstring>::const_iterator itValue;
        map<tstring, tstring> mapMenu = GetRegAllSzValuePairs(hRootKey, lpRegPath);

        for (itValue = mapMenu.begin(); itValue != mapMenu.end(); ++itValue)
        {
            tstring strRegPath = itValue->second;

            if (bEraseHead)
            {
                tstring::size_type pos = strRegPath.find(TEXT('\\'));
                if (pos != strRegPath.npos)
                {
                    strRegPath = strRegPath.substr(pos + 1);
                }
            }

            if (!itValue->first.empty())        //不处理注册表默认值
            {
                TCHAR szShortRegPath[1000];

                lstrcpyn(szShortRegPath, itValue->second.c_str(), RTL_NUMBER_OF(szShortRegPath));
                PathCompactPathHelper(m_hMenuDc, szShortRegPath, 400);

                AppendMenu(hPopupMenu, MF_STRING, *pMenuId, (itValue->first + TEXT("\t") + szShortRegPath).c_str());
                m_mapMenuItems[*pMenuId] = &*m_setMenuItemsRegistry.insert(MenuItemRegistry(*pMenuId, itValue->first.c_str(), strRegPath.c_str())).first;
                *pMenuId += 1;
            }
        }

        return hPopupMenu;
    }

    HMENU InitRegPathSubMenu(UINT *pMenuId)
    {
        LPCTSTR lpSysRegPath = TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit\\Favorites");
        LPCTSTR lpSlxRegPath = TEXT("Software\\Shilyx Studio\\SlxCom\\RegPath");
        LPCTSTR lpSlxRegPath2 = TEXT("HKEY_CURRENT_USER\\Software\\Shilyx Studio\\SlxCom\\RegPath");
        HMENU hRetMenu = CreatePopupMenu();

        HMENU hSysMenu = RegistryPathToMenu(pMenuId, HKEY_CURRENT_USER, lpSysRegPath, TRUE, FALSE);
        AppendMenu(hRetMenu, MF_POPUP, (UINT)hSysMenu, TEXT("注册表编辑器搜藏位置(&F)"));

        HMENU hSlxMenu = RegistryPathToMenu(pMenuId, HKEY_CURRENT_USER, lpSlxRegPath, FALSE, TRUE);
        InsertMenu(hSlxMenu, 0, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
        InsertMenu(hSlxMenu, 0, MF_BYPOSITION, *pMenuId, TEXT("配置SlxCom搜藏(&C)..."));
        m_mapMenuItems[*pMenuId] = &*m_setMenuItemsRegistry.insert(MenuItemRegistry(*pMenuId, NULL, lpSlxRegPath2)).first;
        AppendMenu(hRetMenu, MF_POPUP, (UINT)hSlxMenu, TEXT("SlxCom搜藏位置(&S)"));
        *pMenuId += 1;

        return hRetMenu;
    }

private:
    HWND m_hWindow;
    HMENU m_hMenu;
    HINSTANCE m_hInstance;
    map<int, MenuItem *> m_mapMenuItems;
    set<MenuItemRegistry> m_setMenuItemsRegistry;

private:
    HFONT m_hMenuFont;
    HGDIOBJ m_hOldFont;
    HDC m_hMenuDc;
};

static LRESULT __stdcall NotifyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static NOTIFYICONDATA nid = {sizeof(nid)};
    static int nIconShowIndex = INT_MAX;
    static HICON arrIcons[ICON_COUNT] = {NULL};
    static MenuMgr *pMenuMgr = NULL;

    if (uMsg == WM_CREATE)
    {
        LPCREATESTRUCT lpCs = (LPCREATESTRUCT)lParam;

        //load icons
        int nFirstIconId = IDI_BIRD_0;

        for (int i = 0; i < ICON_COUNT; i += 1)
        {
            arrIcons[i] = LoadIcon(lpCs->hInstance, MAKEINTRESOURCE(nFirstIconId + i));
        }

        //add icon to tray
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uCallbackMessage = WM_CALLBACK;
        nid.hIcon = arrIcons[0];
        nid.uFlags = NIF_TIP | NIF_MESSAGE | NIF_ICON;
        lstrcpyn(nid.szTip, TEXT("SlxCom"), sizeof(nid.szTip));

        Shell_NotifyIcon(NIM_ADD, &nid);

        //
        SetTimer(hWnd, TIMER_ICON, 55, NULL);
        SetTimer(hWnd, TIMER_MENU, 60 * 1000 + 52, NULL);

        PostMessage(hWnd, WM_TIMER, TIMER_MENU, 0);

        //
        pMenuMgr = new MenuMgr(hWnd, lpCs->hInstance);

        return 0;
    }
    else if (uMsg == WM_TIMER)
    {
        if (TIMER_ICON == wParam)
        {
            if (nIconShowIndex < RTL_NUMBER_OF(arrIcons))
            {
                nid.hIcon = arrIcons[nIconShowIndex];
                nid.uFlags = NIF_ICON;

                Shell_NotifyIcon(NIM_MODIFY, &nid);
                nIconShowIndex += 1;
            }
        }
        else if (TIMER_MENU == wParam)
        {
            pMenuMgr->UpdateMenu();
        }
    }
    else if (uMsg == WM_CLOSE)
    {
        Shell_NotifyIcon(NIM_DELETE, &nid);
        DestroyWindow(hWnd);
    }
    else if (uMsg == WM_DESTROY)
    {
        if (pMenuMgr != NULL)
        {
            delete pMenuMgr;
            pMenuMgr = NULL;
        }

        PostQuitMessage(0);
    }
    else if (uMsg == WM_LBUTTONDOWN)
    {
        nIconShowIndex = 0;
    }
    else if (uMsg == WM_CALLBACK)
    {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONDBLCLK)
        {
            POINT pt;

            GetCursorPos(&pt);

            SetForegroundWindow(hWnd);
            pMenuMgr->TrackAndDealPopupMenu(pt.x, pt.y);
        }

        if (nIconShowIndex >= RTL_NUMBER_OF(arrIcons))
        {
            nIconShowIndex = 0;
        }
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static HWND GetTrayNotifyWndInProcess()
{
    DWORD dwPid = GetCurrentProcessId();
    HWND hShell_TrayWnd = FindWindowEx(NULL, NULL, TEXT("Shell_TrayWnd"), NULL);

    while (IsWindow(hShell_TrayWnd))
    {
        DWORD dwWindowPid = 0;

        GetWindowThreadProcessId(hShell_TrayWnd, &dwWindowPid);

        if (dwWindowPid == dwPid)
        {
            HWND hTrayNotifyWnd = FindWindowEx(hShell_TrayWnd, NULL, TEXT("TrayNotifyWnd"), NULL);

            if (IsWindow(hTrayNotifyWnd))
            {
                return hTrayNotifyWnd;
            }
        }

        hShell_TrayWnd = FindWindowEx(NULL, hShell_TrayWnd, TEXT("Shell_TrayWnd"), NULL);
    }

    return NULL;
}

static LRESULT CALLBACK CallWndRetProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_bChangeButtonText)
    {
        LPCWPRETSTRUCT lpCrs = (LPCWPRETSTRUCT)lParam;

        if (lpCrs != NULL && lpCrs->message == WM_INITDIALOG)
        {
            HWND hYes = GetDlgItem(lpCrs->hwnd, IDYES);
            HWND hNo = GetDlgItem(lpCrs->hwnd, IDNO);
            HWND hCancel = GetDlgItem(lpCrs->hwnd, IDCANCEL);

            if (IsWindow(hYes) && IsWindow(hNo) && IsWindow(hCancel))
            {
                SetDlgItemText(lpCrs->hwnd, IDYES, TEXT("就近(&N)"));
                SetDlgItemText(lpCrs->hwnd, IDNO, TEXT("创建(&C)"));
            }
        }
    }

    return CallNextHookEx(g_hMsgHook, nCode, wParam, lParam);
}

static DWORD __stdcall NotifyIconManagerProc(LPVOID lpParam)
{
    if (g_hMsgHook != NULL)
    {
        return 0;
    }

    DWORD dwSleepTime = 1;
    while (!IsWindow(GetTrayNotifyWndInProcess()))
    {
        Sleep((dwSleepTime++) * 1000);
    }

    WNDCLASSEX wcex = {sizeof(wcex)};
    wcex.lpfnWndProc = NotifyWndProc;
    wcex.lpszClassName = NOTIFYWNDCLASS;
    wcex.hInstance = (HINSTANCE)lpParam;
    wcex.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassEx(&wcex);

    HWND hMainWnd = CreateWindowEx(
        0,
        NOTIFYWNDCLASS,
        NOTIFYWNDCLASS,
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        333,
        55,
        NULL,
        NULL,
        (HINSTANCE)lpParam,
        NULL
        );

//     ShowWindow(hMainWnd, SW_SHOW);
//     UpdateWindow(hMainWnd);

    g_hMsgHook = SetWindowsHookEx(WH_CALLWNDPROCRET, CallWndRetProc, (HINSTANCE)lpParam, GetCurrentThreadId());

    MSG msg;

    while (TRUE)
    {
        int nRet = GetMessage(&msg, NULL, 0, 0);

        if (nRet <= 0)
        {
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hMsgHook);
    g_hMsgHook = NULL;

    return 0;
}

void StartNotifyIconManager(HINSTANCE hinstDll)
{
    if (!IsExplorer())
    {
        return;
    }

    if (IsWow64ProcessHelper(GetCurrentProcess()))
    {
        return;
    }

    HANDLE hThread = CreateThread(NULL, 0, NotifyIconManagerProc, (LPVOID)hinstDll, 0, NULL);
    CloseHandle(hThread);
}

void __stdcall T3(HWND hwndStub, HINSTANCE hAppInstance, LPTSTR lpszCmdLine, int nCmdShow)
{
    extern HINSTANCE g_hinstDll; //SlxCom.cpp
    NotifyIconManagerProc((LPVOID)g_hinstDll);
}