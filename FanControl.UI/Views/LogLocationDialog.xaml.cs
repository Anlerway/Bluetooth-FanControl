using System.Windows;
using System.IO;
using FanControl.Shared.Enums;
using FanControl.Shared.Models;

namespace FanControl.UI.Views;

/// <summary>首次启动引导：选择用户数据存放位置（AppData / exe 目录 Userdata），决定 Config\ 与 Logs\ 的归属。</summary>
public partial class LogLocationDialog : Window
{
    public SystemConfig? Result { get; private set; }

    public LogLocationDialog(string installDirectory, string appDataRoot)
    {
        InitializeComponent();
        DataPathText.Text = Path.Combine(appDataRoot);
        InstallPathText.Text = Path.Combine(installDirectory, "Userdata");
    }

    private void Data_Click(object sender, RoutedEventArgs e)
    {
        Result = new SystemConfig { UserDataLocation = ConfigLocation.AppData };
        DialogResult = true;
    }

    private void Install_Click(object sender, RoutedEventArgs e)
    {
        Result = new SystemConfig { UserDataLocation = ConfigLocation.ExeDirectory };
        DialogResult = true;
    }
}
