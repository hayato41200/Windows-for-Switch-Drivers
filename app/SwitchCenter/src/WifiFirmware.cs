// Wi-Fi (switchwlan) and Bluetooth (switchbt) firmware on the running Switch.
//
// The files are the user's own (from their Switchroot Linux install); see
// app\Shared\SwitchFirmware.cs for what goes where. The elevated
// "--wifi-firmware <folder>" mode copies the Wi-Fi and Bluetooth files into this
// Windows, writes wifi-mac.txt to Services\switchwlan\Parameters\MacAddress,
// and restarts the Wi-Fi device. The XUSB (USB) firmware lives on the ESP and is
// SwitchPatcher's job, not this one's. The folders are outside the driver store,
// so driver updates keep the files.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    static class WifiFirmware
    {
        public const string Service = "switchwlan";
        const string Hwid = "SWOA0D01";

        static string WindowsRoot => Path.GetPathRoot(Environment.SystemDirectory);

        public static bool Present() => SwitchFirmware.WifiPresent(WindowsRoot);
        public static bool BluetoothPresent() => SwitchFirmware.BtPresent(WindowsRoot);

        /// <summary>Starts this program elevated to copy the files from folder. null = the user said no to UAC.</summary>
        public static async Task<int?> RunElevatedAsync(string folder)
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath, $"--wifi-firmware \"{folder.TrimEnd('\\')}\"")
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            Process p;
            try { p = Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return null; }
            using (p)
            {
                await p.WaitForExitAsync();
                return p.ExitCode;
            }
        }

        /// <summary>The elevated part. Returns 0 when something was put in place.</summary>
        public static int Apply(string source)
        {
            var log = new List<string>();
            int code = 1;
            try
            {
                if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));
                FirmwareSet f = SwitchFirmware.Find(source);
                log.AddRange(SwitchFirmware.Describe(f, false));
                if (!f.Wifi && f.BtHcd == null && f.Mac == null)
                    throw new FileNotFoundException(T("入れられるファイルがありません。Switchroot Linux の /lib/firmware/brcm から brcmfmac4356A3-pcie.bin、.txt、BCM4356A3.hcd をこのフォルダーにコピーしてください。",
                                                      "Nothing to add. Copy brcmfmac4356A3-pcie.bin, .txt and BCM4356A3.hcd from Switchroot Linux's /lib/firmware/brcm into this folder."));
                log.Add("");
                SwitchFirmware.CopyToWindows(f, WindowsRoot, log.Add);
                if (f.Mac != null)
                {
                    using (RegistryKey k = Registry.LocalMachine.CreateSubKey(@"SYSTEM\CurrentControlSet\" + SwitchFirmware.WlanParams))
                        k.SetValue("MacAddress", f.Mac, RegistryValueKind.String);
                    log.Add(T("Wi-Fi の MAC アドレスを設定しました。", "Set the Wi-Fi MAC address."));
                }
                code = 0;
                if (f.Wifi || f.Mac != null)
                    log.Add(RestartDevice(log) ? T("Wi-Fi のデバイスを再起動しました。少し待つとネットワークが見えます。", "Restarted the Wi-Fi device; networks show up in a moment.")
                                               : T("Windows を再起動すると、Wi-Fi が新しいファイルで動きます。", "Restart Windows to use the new Wi-Fi files."));
                if (f.BtHcd != null)
                    log.Add(T("Bluetooth のパッチは、Windows を再起動したときから使われます。", "The Bluetooth patch is used from the next Windows restart."));
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException)
            {
                log.Add(T("失敗しました: ", "Failed: ") + e.Message);
            }
            MessageBox.Show(string.Join(Environment.NewLine, log), T("ファームウェア", "Firmware"),
                            MessageBoxButtons.OK, code == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Error);
            return code;
        }

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] static extern int CM_Locate_DevNodeW(out uint devInst, string id, uint flags);

        /// <summary>Restarts the present Wi-Fi device instances so switchwlan reads the new files. false = none restarted.</summary>
        static bool RestartDevice(List<string> log)
        {
            string[] instances;
            using (RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\ACPI\{Hwid}"))
                instances = k?.GetSubKeyNames() ?? Array.Empty<string>();
            bool any = false;
            StopWlanService(log.Add);
            try
            {
                foreach (string inst in instances)
                {
                    string id = $@"ACPI\{Hwid}\{inst}";
                    if (CM_Locate_DevNodeW(out _, id, 0) != 0) continue;       // not present now
                    var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "pnputil.exe"), $"/restart-device \"{id}\"")
                    { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                    using Process p = Process.Start(psi);
                    p.StandardOutput.ReadToEnd();
                    if (!p.WaitForExit(120_000)) return false;
                    if (p.ExitCode == 0) any = true;
                }
            }
            finally { StartWlanService(log.Add); }
            return any;
        }

        // WLAN AutoConfig keeps the adapter open: stop it while the driver is replaced or restarted.
        public static void StopWlanService(Action<string> log) => Sc("stop", "STOPPED", log);
        public static void StartWlanService(Action<string> log) => Sc("start", "RUNNING", log);

        static void Sc(string verb, string want, Action<string> log)
        {
            string sc = Path.Combine(Environment.SystemDirectory, "sc.exe");
            Run(sc, $"{verb} wlansvc");
            for (int i = 0; i < 30; i++)
            {
                if (Run(sc, "query wlansvc").Contains(want)) return;
                Thread.Sleep(500);
            }
            log(T($"  WLAN AutoConfig（wlansvc）が {want} になりませんでした。", $"  WLAN AutoConfig (wlansvc) did not reach {want}."));
        }

        static string Run(string exe, string args)
        {
            var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
            using Process p = Process.Start(psi);
            string o = p.StandardOutput.ReadToEnd();
            p.WaitForExit(10_000);
            return o;
        }
    }
}
