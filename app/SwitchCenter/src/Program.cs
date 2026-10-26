// Switch control center (stage 1: notification-area icon). See
// Windows-Drivers\Design-ControlCenter-ja.md for the plan.
using System;
using System.Threading;
using System.Windows.Forms;

namespace SwitchCenter
{
    static class Program
    {
        /// <param name="args">--open: show the control center window right away.</param>
        [STAThread]
        static void Main(string[] args)
        {
            using var single = new Mutex(true, @"Local\SwitchWOA.SwitchCenter", out bool created);
            if (!created) return;
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Lang.Load();
            Application.Run(new TrayApp(Array.IndexOf(args, "--open") >= 0));
        }
    }
}
