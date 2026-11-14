using System.Runtime.InteropServices;

namespace FanControl.Service.Tray;

/// <summary>
/// 同进程托盘：直接调用 Shell_NotifyIcon 与原生弹出菜单，完全不依赖 System.Windows.Forms。
/// 在独立线程上运行 Win32 消息泵，与 UI 线程通过 PostMessage 协作。
/// 去掉 WinForms 引用后，Microsoft.WindowsDesktop.App（约 93.7 MB）不再进入依赖链。
/// </summary>
public sealed class TrayHost : IDisposable
{
    private Thread? _thread;
    private TrayWindow? _window;

    /// <summary>启动托盘线程。三个回调分别在双击/显示窗口、退出、重建连接时触发。</summary>
    public void Start(Action showUi, Action exit, Action? reconnect = null)
    {
        _window = new TrayWindow(showUi, exit, reconnect);
        _thread = new Thread(_window.Run)
        {
            IsBackground = true,
            Name = "FanControl.Tray",
        };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    /// <summary>更新托盘图标悬停提示（跨线程安全）。</summary>
    public void SetStatus(string text)
    {
        _window?.SetStatus(text);
    }

    /// <summary>弹出托盘气泡通知（跨线程安全）。</summary>
    public void ShowNotification(string title, string text)
    {
        _window?.ShowNotification(title, text);
    }

    public void Dispose()
    {
        _window?.Shutdown();
        _thread?.Join(TimeSpan.FromSeconds(2));
        _window = null;
    }
}

/// <summary>托盘线程本体：隐藏窗口 + 消息泵 + Shell_NotifyIcon。</summary>
internal sealed class TrayWindow
{
    private const int WmTrayIcon = Native.WmApp + 1;
    private const int WmSetStatus = Native.WmApp + 2;
    private const int WmShowNotify = Native.WmApp + 3;

    private const int CmdOpen = 1;
    private const int CmdReconnect = 2;
    private const int CmdAutostart = 3;
    private const int CmdExit = 4;

    private const uint TrayId = 1;
    private const int TipMaxLength = 127;

    private static TrayWindow? s_instance;
    private static readonly Native.WndProc s_windowProc = StaticWindowProc;
    private static bool s_classRegistered;

    private readonly Action _showUi;
    private readonly Action _exit;
    private readonly Action? _reconnect;
    private readonly object _sync = new();
    private readonly uint _taskbarCreatedMessage = Native.RegisterWindowMessageW("TaskbarCreated");

    private IntPtr _hwnd;
    private IntPtr _icon;
    private string _tip = "FanControl";
    private (string Title, string Text)? _pendingNotification;
    private bool _autostartEnabled;
    private bool _iconAdded;
    private bool _shutdownRequested;

    public TrayWindow(Action showUi, Action exit, Action? reconnect)
    {
        _showUi = showUi;
        _exit = exit;
        _reconnect = reconnect;
    }

    public void Run()
    {
        if (!EnsureWindow())
        {
            return;
        }

        s_instance = this;
        AddIcon();
        RefreshAutostartState();

        while (!_shutdownRequested && Native.GetMessageW(out var msg, IntPtr.Zero, 0, 0) > 0)
        {
            Native.TranslateMessage(ref msg);
            Native.DispatchMessageW(ref msg);
        }

        RemoveIcon();
        if (_hwnd != IntPtr.Zero)
        {
            Native.DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }

        s_instance = null;
    }

    public void SetStatus(string text)
    {
        var tip = string.IsNullOrWhiteSpace(text) ? "FanControl" : text.Trim();
        if (tip.Length > TipMaxLength)
        {
            tip = tip[..TipMaxLength];
        }

        lock (_sync)
        {
            _tip = tip;
        }

        PostIfReady(WmSetStatus);
    }

    public void ShowNotification(string title, string text)
    {
        lock (_sync)
        {
            _pendingNotification = (title, text);
        }

        PostIfReady(WmShowNotify);
    }

    public void Shutdown()
    {
        _shutdownRequested = true;
        if (_hwnd != IntPtr.Zero)
        {
            Native.PostMessageW(_hwnd, Native.WmClose, IntPtr.Zero, IntPtr.Zero);
        }
    }

    private void PostIfReady(int message)
    {
        var hwnd = _hwnd;
        if (hwnd != IntPtr.Zero)
        {
            Native.PostMessageW(hwnd, (uint)message, IntPtr.Zero, IntPtr.Zero);
        }
    }

    private bool EnsureWindow()
    {
        if (!s_classRegistered)
        {
            var wc = new Native.WndClassEx
            {
                cbSize = (uint)Marshal.SizeOf<Native.WndClassEx>(),
                lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_windowProc),
                hInstance = Native.GetModuleHandleW(null),
                lpszClassName = "FanControlTrayWindow",
            };

            if (Native.RegisterClassExW(ref wc) == 0)
            {
                return false;
            }

            s_classRegistered = true;
        }

        // 普通隐藏窗口（不是 message-only），这样能收到 TaskbarCreated 广播，explorer 重启后可重新加图标。
        _hwnd = Native.CreateWindowExW(
            0,
            "FanControlTrayWindow",
            "FanControl",
            0,
            0,
            0,
            0,
            0,
            IntPtr.Zero,
            IntPtr.Zero,
            Native.GetModuleHandleW(null),
            IntPtr.Zero);

        return _hwnd != IntPtr.Zero;
    }

    private static IntPtr StaticWindowProc(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam)
    {
        var instance = s_instance;
        if (instance is null)
        {
            return Native.DefWindowProcW(hwnd, message, wParam, lParam);
        }

        return instance.HandleMessage(hwnd, message, wParam, lParam);
    }

    private IntPtr HandleMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == _taskbarCreatedMessage)
        {
            _iconAdded = false;
            AddIcon();
            return IntPtr.Zero;
        }

        switch ((int)message)
        {
            case WmTrayIcon:
                switch ((int)(lParam.ToInt64() & 0xFFFF))
                {
                    case Native.WmLButtonDblClk:
                        _showUi();
                        break;
                    case Native.WmRButtonUp:
                        ShowMenu();
                        break;
                }

                return IntPtr.Zero;

            case WmSetStatus:
                UpdateTip();
                return IntPtr.Zero;

            case WmShowNotify:
                ShowBalloon();
                return IntPtr.Zero;

            case Native.WmClose:
                _shutdownRequested = true;
                RemoveIcon();
                Native.DestroyWindow(hwnd);
                _hwnd = IntPtr.Zero;
                Native.PostQuitMessage(0);
                return IntPtr.Zero;
        }

        return Native.DefWindowProcW(hwnd, message, wParam, lParam);
    }

    private void ShowMenu()
    {
        Native.GetCursorPos(out var point);

        var menu = Native.CreatePopupMenu();
        if (menu == IntPtr.Zero)
        {
            return;
        }

        Native.AppendMenuW(menu, Native.MfString, CmdOpen, TrayStrings.OpenUi);
        Native.AppendMenuW(menu, Native.MfSeparator, 0, null);
        Native.AppendMenuW(menu, Native.MfString, CmdReconnect, TrayStrings.Reconnect);
        Native.AppendMenuW(menu, Native.MfSeparator, 0, null);

        var autostartFlags = Native.MfString;
        if (_autostartEnabled)
        {
            autostartFlags |= Native.MfChecked;
        }

        Native.AppendMenuW(menu, autostartFlags, CmdAutostart, TrayStrings.Autostart);
        Native.AppendMenuW(menu, Native.MfSeparator, 0, null);
        Native.AppendMenuW(menu, Native.MfString, CmdExit, TrayStrings.Exit);

        // TrackPopupMenu 要求菜单所属窗口为前台，否则点空白处菜单不会消失
        Native.SetForegroundWindow(_hwnd);
        var command = Native.TrackPopupMenuEx(
            menu,
            Native.TpmReturnCmd | Native.TpmRightButton,
            point.X,
            point.Y,
            _hwnd,
            IntPtr.Zero);
        Native.DestroyMenu(menu);
        Native.PostMessageW(_hwnd, Native.WmNull, IntPtr.Zero, IntPtr.Zero);

        switch (command)
        {
            case CmdOpen:
                _showUi();
                break;
            case CmdReconnect:
                _reconnect?.Invoke();
                break;
            case CmdAutostart:
                _ = ToggleAutostartAsync();
                break;
            case CmdExit:
                _exit();
                break;
        }
    }

    private void AddIcon()
    {
        if (_icon == IntPtr.Zero)
        {
            _icon = LoadProcessIcon();
        }

        var data = CreateIconData();
        data.uFlags = Native.NifMessage | Native.NifIcon | Native.NifTip;
        data.uCallbackMessage = WmTrayIcon;

        if (!Native.ShellNotifyIcon(Native.NimAdd, ref data))
        {
            return;
        }

        _iconAdded = true;
    }

    private void RemoveIcon()
    {
        if (!_iconAdded)
        {
            return;
        }

        var data = CreateIconData();
        Native.ShellNotifyIcon(Native.NimDelete, ref data);
        _iconAdded = false;
    }

    private void UpdateTip()
    {
        if (!_iconAdded)
        {
            return;
        }

        string tip;
        lock (_sync)
        {
            tip = _tip;
        }

        var data = CreateIconData();
        data.uFlags = Native.NifTip;
        data.szTip = tip;
        Native.ShellNotifyIcon(Native.NimModify, ref data);
    }

    private void ShowBalloon()
    {
        (string Title, string Text)? payload;
        lock (_sync)
        {
            payload = _pendingNotification;
            _pendingNotification = null;
        }

        if (payload is null || !_iconAdded)
        {
            return;
        }

        var data = CreateIconData();
        data.uFlags = Native.NifInfo;
        data.szInfoTitle = payload.Value.Title;
        data.szInfo = payload.Value.Text;
        data.dwInfoFlags = Native.NiifWarning;
        data.uTimeout = 3000;
        Native.ShellNotifyIcon(Native.NimModify, ref data);
    }

    private Native.NotifyIconData CreateIconData()
    {
        string tip;
        lock (_sync)
        {
            tip = _tip;
        }

        return new Native.NotifyIconData
        {
            cbSize = (uint)Marshal.SizeOf<Native.NotifyIconData>(),
            hWnd = _hwnd,
            uID = TrayId,
            hIcon = _icon,
            szTip = tip,
            szInfo = string.Empty,
            szInfoTitle = string.Empty,
        };
    }

    private static IntPtr LoadProcessIcon()
    {
        // 优先取主程序 exe 内嵌的 ApplicationIcon，失败时回退系统默认应用图标
        var path = Environment.ProcessPath;
        if (!string.IsNullOrEmpty(path)
            && Native.ExtractIconExW(path, 0, out var large, out _, 1) > 0
            && large != IntPtr.Zero)
        {
            return large;
        }

        return Native.LoadIconW(IntPtr.Zero, Native.IdiApplication);
    }

    private void RefreshAutostartState()
    {
        _ = Task.Run(async () =>
        {
            try
            {
                _autostartEnabled = await AutostartManager.IsEnabledAsync();
            }
            catch
            {
                // 读取失败时保持上一次状态
            }
        });
    }

    private async Task ToggleAutostartAsync()
    {
        try
        {
            if (_autostartEnabled)
            {
                await AutostartManager.DisableAsync();
                _autostartEnabled = false;
            }
            else
            {
                await AutostartManager.EnableAsync();
                _autostartEnabled = true;
            }
        }
        catch (Exception ex)
        {
            Native.MessageBoxW(IntPtr.Zero, TrayStrings.AutostartFailed(ex.Message), "FanControl", Native.MbOk | Native.MbIconWarning);
        }
    }
}

/// <summary>user32 / shell32 的最小互操作声明。</summary>
internal static class Native
{
    public const int WmApp = 0x8000;
    public const int WmClose = 0x0010;
    public const int WmNull = 0x0000;
    public const int WmLButtonDblClk = 0x0203;
    public const int WmRButtonUp = 0x0205;

    public const uint NifMessage = 0x00000001;
    public const uint NifIcon = 0x00000002;
    public const uint NifTip = 0x00000004;
    public const uint NifInfo = 0x00000010;
    public const uint NimAdd = 0;
    public const uint NimModify = 1;
    public const uint NimDelete = 2;
    public const uint NiifWarning = 0x00000002;

    public const uint MfString = 0x00000000;
    public const uint MfChecked = 0x00000008;
    public const uint MfSeparator = 0x00000800;
    public const uint TpmRightButton = 0x0002;
    public const uint TpmReturnCmd = 0x0100;

    public const uint MbOk = 0x00000000;
    public const uint MbIconWarning = 0x00000030;

    public static readonly IntPtr IdiApplication = new(32512);

    public delegate IntPtr WndProc(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct Point
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Msg
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public Point pt;
        public uint lPrivate;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WndClassEx
    {
        public uint cbSize;
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct NotifyIconData
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uID;
        public uint uFlags;
        public int uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState;
        public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uTimeout;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassExW(ref WndClassEx lpwcx);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(
        uint dwExStyle,
        string lpClassName,
        string lpWindowName,
        uint dwStyle,
        int x,
        int y,
        int nWidth,
        int nHeight,
        IntPtr hWndParent,
        IntPtr hMenu,
        IntPtr hInstance,
        IntPtr lpParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr DefWindowProcW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern int GetMessageW(out Msg lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref Msg lpMsg);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr DispatchMessageW(ref Msg lpMsg);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern void PostQuitMessage(int exitCode);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern uint RegisterWindowMessageW(string lpString);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out Point lpPoint);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool AppendMenuW(IntPtr hMenu, uint uFlags, int uIDNewItem, string? lpNewItem);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyMenu(IntPtr hMenu);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern int TrackPopupMenuEx(IntPtr hMenu, uint uFlags, int x, int y, IntPtr hWnd, IntPtr lptpm);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadIconW(IntPtr hInstance, IntPtr lpIconName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int MessageBoxW(IntPtr hWnd, string lpText, string lpCaption, uint uType);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconExW(string lpszFile, int nIconIndex, out IntPtr phiconLarge, out IntPtr phiconSmall, uint nIcons);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool ShellNotifyIcon(uint dwMessage, ref NotifyIconData lpData);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandleW(string? lpModuleName);
}
