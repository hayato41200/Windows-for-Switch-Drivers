// Wi-Fi (switchwlan) firmware, NVRAM and the console's MAC address.
//
// None of these are in a release: the Broadcom firmware and the board NVRAM
// are not ours to redistribute, and the MAC belongs to one console. The user
// points SwitchCenter at a folder with their own copies (for example taken from
// their Switchroot / L4T install: /lib/firmware/brcm/brcmfmac4356A3-pcie.bin
// and .txt), and the elevated "--wifi-firmware <folder>" mode copies them to
// System32\drivers\switchwifi, where switchwlan reads them at start. That
// folder is outside the driver store, so driver updates keep it.
// wifi-mac.txt ("xx:xx:xx:xx:xx:xx") in the same folder, if present, goes to
// Services\switchwlan\Parameters\MacAddress (the driver writes it into the
// NVRAM "macaddr" before the firmware boots).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.RegularExpressions;
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
        const string ParamsKey = @"SYSTEM\CurrentControlSet\Services\switchwlan\Parameters";
        const string BinName = "brcmfmac4356-pcie.bin", TxtName = "brcmfmac4356-pcie.txt";
        // The firmware the driver was tested with (7.35.252.108, from Switchroot L4T). Only its hash is here.
        const string TestedBinSha256 = "32a16e7b4391ccd248d3677c17cde940f398aaaaa6163571fcbff2bbee026ade";

        public static string Folder => Path.Combine(Environment.SystemDirectory, "drivers", "switchwifi");

        public static bool Present() =>
            File.Exists(Path.Combine(Folder, BinName)) && File.Exists(Path.Combine(Folder, TxtName));

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

        /// <summary>The elevated part. Returns 0 when the files are in place.</summary>
        public static int Apply(string source)
        {
            var log = new List<string>();
            int code = 1;
            try
            {
                if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));
                string bin = Find(source, BinName, "brcmfmac4356A3-pcie.bin");
                string txt = Find(source, TxtName, "brcmfmac4356A3-pcie.txt");
                if (bin == null || txt == null)
                    throw new FileNotFoundException(T(
                        "フォルダーに brcmfmac4356-pcie.bin と .txt（または brcmfmac4356A3-pcie.bin と .txt）がありません。",
                        "The folder has no brcmfmac4356-pcie.bin and .txt (or brcmfmac4356A3-pcie.bin and .txt)."));
                long binLen = new FileInfo(bin).Length, txtLen = new FileInfo(txt).Length;
                if (binLen < 100_000 || binLen > 2_000_000 || txtLen < 100 || txtLen > 64 * 1024)
                    throw new InvalidDataException(T($"ファイルの大きさがおかしいです（.bin {binLen} バイト、.txt {txtLen} バイト）。",
                                                     $"Unexpected file sizes (.bin {binLen} bytes, .txt {txtLen} bytes)."));
                string hash;
                using (FileStream f = File.OpenRead(bin)) hash = Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
                log.Add(hash == TestedBinSha256
                    ? T("ファームウェア: 動作を確かめた版（7.35.252.108）です。", "Firmware: the tested version (7.35.252.108).")
                    : T("ファームウェア: 動作を確かめた版とは違います。動かないときは Switchroot の brcmfmac4356A3-pcie.bin を使ってください。",
                        "Firmware: not the tested version. If Wi-Fi does not work, use Switchroot's brcmfmac4356A3-pcie.bin."));

                Directory.CreateDirectory(Folder);
                File.Copy(bin, Path.Combine(Folder, BinName), true);
                File.Copy(txt, Path.Combine(Folder, TxtName), true);
                log.Add(T($"{Folder} にコピーしました。", $"Copied to {Folder}."));

                string macFile = Path.Combine(source, "wifi-mac.txt");
                if (File.Exists(macFile))
                {
                    string mac = File.ReadAllLines(macFile).Length > 0 ? Regex.Replace(File.ReadAllLines(macFile)[0], @"\s", "").ToUpperInvariant().Replace('-', ':') : "";
                    if (!Regex.IsMatch(mac, "^([0-9A-F]{2}:){5}[0-9A-F]{2}$"))
                        throw new InvalidDataException(T("wifi-mac.txt が xx:xx:xx:xx:xx:xx の形ではありません。", "wifi-mac.txt is not xx:xx:xx:xx:xx:xx."));
                    using (RegistryKey k = Registry.LocalMachine.CreateSubKey(ParamsKey)) k.SetValue("MacAddress", mac, RegistryValueKind.String);
                    log.Add(T($"MAC アドレス（{mac.Substring(0, 8)}:xx:xx:xx）を設定しました。", $"Set the MAC address ({mac.Substring(0, 8)}:xx:xx:xx)."));
                }
                else
                {
                    log.Add(T("wifi-mac.txt が無いので、MAC アドレスは変えていません。", "No wifi-mac.txt; the MAC address is unchanged."));
                }

                code = 0;
                if (RestartDevice(log)) log.Add(T("Wi-Fi のデバイスを再起動しました。少し待つとネットワークが見えます。",
                                                  "Restarted the Wi-Fi device; networks show up in a moment."));
                else log.Add(T("Windows を再起動すると、Wi-Fi が使えるようになります。", "Restart Windows to start Wi-Fi."));
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException || e is ArgumentException)
            {
                log.Add(T("失敗しました: ", "Failed: ") + e.Message);
            }
            MessageBox.Show(string.Join(Environment.NewLine, log), T("Wi-Fi のファームウェア", "Wi-Fi firmware"),
                            MessageBoxButtons.OK, code == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Error);
            return code;
        }

        /// <summary>name or alt in folder or folder\firmware.</summary>
        static string Find(string folder, string name, string alt)
        {
            foreach (string dir in new[] { folder, Path.Combine(folder, "firmware") })
                foreach (string n in new[] { name, alt })
                {
                    string p = Path.Combine(dir, n);
                    if (File.Exists(p)) return p;
                }
            return null;
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
