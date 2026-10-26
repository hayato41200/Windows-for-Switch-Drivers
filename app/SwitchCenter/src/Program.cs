// Switch control center (stage 1: notification-area icon). See
// Windows-Drivers\Design-ControlCenter-ja.md for the plan.
using System;
using System.Threading;
using System.Windows.Forms;

namespace SwitchCenter
{
    static class Program
    {
        /// <param name="args">--open: show the control center window right away.
        /// --install-drivers &lt;tag&gt; &lt;service,...&gt;: the elevated driver installer.</param>
        [STAThread]
        static void Main(string[] args)
        {
            if (args.Length == 3 && args[0] == "--install-drivers")
            {
                // Elevated OTA installer started by the Drivers tab (Updater.RunElevatedAsync).
                Application.EnableVisualStyles();
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Lang.Load();
                using var form = new InstallForm(args[1], args[2].Split(','));
                Application.Run(form);
                Environment.ExitCode = form.Result;
                return;
            }
            using var single = new Mutex(true, @"Local\SwitchWOA.SwitchCenter", out bool created);
            if (!created) return;
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Lang.Load();
            Application.Run(new TrayApp(Array.IndexOf(args, "--open") >= 0));
        }
    }
}
