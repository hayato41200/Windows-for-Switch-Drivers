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
        /// --install-drivers &lt;tag&gt; &lt;service,...&gt;: the elevated driver installer.
        /// --self-update &lt;folder&gt; &lt;pid&gt;: an unpacked new version replacing the one in folder.
        /// --wifi-firmware &lt;folder&gt;: the elevated copy of the user's Wi-Fi firmware files.
        /// --update-uefi &lt;tag&gt;: the elevated UEFI (bl33.bin) update for the hekate chain.
        /// --gpu on|off: the elevated GPU drawing switch (applies at the next restart).</param>
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
            if (args.Length == 2 && args[0] == "--update-uefi")
            {
                // Elevated UEFI (bl33.bin, hekate chain) update started by the Drivers tab (UefiUpdate.RunElevatedAsync).
                Application.EnableVisualStyles();
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Lang.Load();
                using var form = new UefiUpdateForm(args[1]);
                Application.Run(form);
                Environment.ExitCode = form.Result;
                return;
            }
            if (args.Length == 2 && args[0] == "--wifi-firmware")
            {
                // Elevated copy of the user's own Wi-Fi firmware, NVRAM and MAC (WifiFirmware.RunElevatedAsync).
                Application.EnableVisualStyles();
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Lang.Load();
                Environment.ExitCode = WifiFirmware.Apply(args[1]);
                return;
            }
            if (args.Length == 2 && args[0] == "--gpu" && (args[1] == "on" || args[1] == "off"))
            {
                // Elevated GPU drawing switch (GpuAccel.RunElevatedAsync).
                Application.EnableVisualStyles();
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Lang.Load();
                Environment.ExitCode = GpuAccel.Apply(args[1] == "on");
                return;
            }
            if (args.Length == 3 && args[0] == "--self-update" && int.TryParse(args[2], out int pid))
            {
                // Unpacked new version replacing the installed one (Updater.StartSelfUpdate).
                Lang.Load();
                Updater.ApplySelfUpdate(args[1], pid);
                return;
            }
            using var single = new Mutex(true, @"Local\SwitchWOA.SwitchCenter", out bool created);
            if (!created) return;
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Lang.Load();
            Updater.CleanUpdates();
            Application.Run(new TrayApp(Array.IndexOf(args, "--open") >= 0));
        }
    }
}
