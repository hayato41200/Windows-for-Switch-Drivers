// Firmware the user brings from their own Switchroot Linux install. None of it is
// in this repository or in a release (Broadcom's and NVIDIA's files, and the MAC
// address of one console). SwitchPatcher (PC) puts all of it on the SD;
// SwitchCenter (on the Switch) can add or replace the Wi-Fi and Bluetooth files.
//
//   file                         goes to                                      read by
//   tegra21x_xusb_firmware       ESP root                                     the UEFI (USB host)
//   brcmfmac4356(A3)-pcie.bin    Windows\System32\drivers\switchwifi\...bin   switchwlan (Wi-Fi)
//   brcmfmac4356(A3)-pcie.txt    Windows\System32\drivers\switchwifi\...txt   switchwlan (NVRAM)
//   BCM4356A3.hcd                Windows\System32\drivers\SwitchWOA\          switchbt (Bluetooth patch)
//   wifi-mac.txt                 Services\switchwlan\Parameters\MacAddress    switchwlan (the console's MAC)
//   nvidia\gm20b\(acr,gr,pmu)    SwitchWOA\firmware\nvidia\gm20b\             the GPU kit (Gpu.ps1 Install imports it)
//
// In Switchroot the files are in /lib/firmware (tegra21x_xusb_firmware) and
// /lib/firmware/brcm (the Broadcom ones). The user copies them into one folder;
// sub-folders named firmware, brcm, lib\firmware(\brcm) and nvidia\tegra210 are searched too.
// The GPU files keep linux-firmware's layout: nvidia\gm20b\acr, gr and pmu.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    sealed class FirmwareSet
    {
        public string Folder;
        public string WifiBin, WifiTxt, BtHcd, Xusb;    // full paths, null when not found
        public string Gpu;                              // the gm20b folder (with acr, gr, pmu), null when not found
        public string Mac;                              // "XX:XX:XX:XX:XX:XX" or null
        public readonly List<string> Problems = new List<string>();     // found but not usable
        public bool Wifi => WifiBin != null && WifiTxt != null;
        public bool Any => Wifi || BtHcd != null || Xusb != null || Mac != null || Gpu != null;
    }

    static class SwitchFirmware
    {
        public const string WifiDir = @"Windows\System32\drivers\switchwifi";     // relative to a Windows partition root
        public const string BtDir = @"Windows\System32\drivers\SwitchWOA";
        public const string WifiBinName = "brcmfmac4356-pcie.bin", WifiTxtName = "brcmfmac4356-pcie.txt";
        public const string BtName = "BCM4356A3.hcd", XusbName = "tegra21x_xusb_firmware";
        public const string WlanParams = @"Services\switchwlan\Parameters";      // under a control set
        public const string GpuDir = @"SwitchWOA\firmware\nvidia\gm20b";          // where kits\Switch-Gpu\Gpu.ps1 looks on C:
        // The files Gpu.ps1 imports (sw_method_init.bin is not in linux-firmware's gm20b and is not needed).
        static readonly string[] GpuFiles = {
            @"acr\bl.bin", @"acr\ucode_load.bin", @"pmu\desc.bin", @"pmu\image.bin", @"pmu\sig.bin",
            @"gr\fecs_bl.bin", @"gr\fecs_inst.bin", @"gr\fecs_data.bin", @"gr\fecs_sig.bin",
            @"gr\gpccs_inst.bin", @"gr\gpccs_data.bin", @"gr\sw_ctx.bin", @"gr\sw_nonctx.bin", @"gr\sw_bundle_init.bin" };
        static readonly string[] GpuFolders = { @"nvidia\gm20b", "gm20b", @"firmware\nvidia\gm20b", @"lib\firmware\nvidia\gm20b", "" };
        // The versions the drivers were tested with. Only hashes; the files are not here.
        const string TestedWifiSha256 = "32a16e7b4391ccd248d3677c17cde940f398aaaaa6163571fcbff2bbee026ade";   // 7.35.252.108

        static readonly string[] SubFolders = { "", "firmware", "brcm", @"lib\firmware", @"lib\firmware\brcm", @"nvidia\tegra210", @"firmware\brcm" };

        /// <summary>Looks for every file in folder (and its usual sub-folders) and checks what it finds.</summary>
        public static FirmwareSet Find(string folder)
        {
            var f = new FirmwareSet { Folder = folder };
            f.WifiBin = Pick(folder, WifiBinName, "brcmfmac4356A3-pcie.bin");
            f.WifiTxt = Pick(folder, WifiTxtName, "brcmfmac4356A3-pcie.txt");
            f.BtHcd = Pick(folder, BtName, "bcm4356a3.hcd");
            f.Xusb = Pick(folder, XusbName, "xusb.bin");

            if (f.WifiBin != null && !SizeOk(f.WifiBin, 100_000, 2_000_000)) { f.Problems.Add(T("Wi-Fi の .bin の大きさがおかしい", "the Wi-Fi .bin has an odd size")); f.WifiBin = null; }
            if (f.WifiTxt != null && !SizeOk(f.WifiTxt, 100, 64 * 1024)) { f.Problems.Add(T("Wi-Fi の .txt の大きさがおかしい", "the Wi-Fi .txt has an odd size")); f.WifiTxt = null; }
            if (f.BtHcd != null && !IsHcd(f.BtHcd)) { f.Problems.Add(T("BCM4356A3.hcd が Broadcom のパッチの形ではない", "BCM4356A3.hcd is not a Broadcom patch file")); f.BtHcd = null; }
            if (f.Xusb != null && !SizeOk(f.Xusb, 64 * 1024, 512 * 1024)) { f.Problems.Add(T("XUSB のファームウェアの大きさがおかしい", "the XUSB firmware has an odd size")); f.Xusb = null; }

            foreach (string sub in GpuFolders)
            {
                string dir = Path.Combine(folder, sub);
                if (!File.Exists(Path.Combine(dir, @"acr\bl.bin"))) continue;
                string[] missing = GpuFiles.Where(n => !File.Exists(Path.Combine(dir, n)) || new FileInfo(Path.Combine(dir, n)).Length == 0).ToArray();
                if (missing.Length == 0) f.Gpu = dir;
                else f.Problems.Add(T("GPU のファームウェアが足りない: ", "the GPU firmware is incomplete: ") + string.Join(", ", missing));
                break;
            }

            string macFile = Path.Combine(folder, "wifi-mac.txt");
            if (File.Exists(macFile))
            {
                string line = File.ReadLines(macFile).FirstOrDefault() ?? "";
                string mac = Regex.Replace(line, @"\s", "").ToUpperInvariant().Replace('-', ':');
                if (Regex.IsMatch(mac, "^([0-9A-F]{2}:){5}[0-9A-F]{2}$") && (Convert.ToByte(mac.Substring(0, 2), 16) & 1) == 0 && mac != "00:00:00:00:00:00") f.Mac = mac;
                else f.Problems.Add(T("wifi-mac.txt が xx:xx:xx:xx:xx:xx の形ではない", "wifi-mac.txt is not xx:xx:xx:xx:xx:xx"));
            }
            return f;
        }

        /// <summary>One line per item for the log or a message box.</summary>
        public static IEnumerable<string> Describe(FirmwareSet f, bool withXusb)
        {
            yield return f.Wifi
                ? T("Wi-Fi: あり", "Wi-Fi: found") + (IsTestedWifi(f.WifiBin) ? T("（動作を確かめた 7.35.252.108）", " (the tested 7.35.252.108)") : T("（動作を確かめた版とは違う）", " (not the tested version)"))
                : T("Wi-Fi: なし（brcmfmac4356A3-pcie.bin と .txt）", "Wi-Fi: not found (brcmfmac4356A3-pcie.bin and .txt)");
            yield return f.BtHcd != null ? T("Bluetooth: あり", "Bluetooth: found") : T("Bluetooth: なし（BCM4356A3.hcd）", "Bluetooth: not found (BCM4356A3.hcd)");
            if (withXusb) yield return f.Xusb != null ? T("USB（XUSB）: あり", "USB (XUSB): found") : T("USB（XUSB）: なし（tegra21x_xusb_firmware）", "USB (XUSB): not found (tegra21x_xusb_firmware)");
            yield return f.Mac != null ? T($"MAC アドレス: {f.Mac.Substring(0, 8)}:xx:xx:xx", $"MAC address: {f.Mac.Substring(0, 8)}:xx:xx:xx") : T("MAC アドレス: なし（wifi-mac.txt）", "MAC address: not found (wifi-mac.txt)");
            yield return f.Gpu != null ? T("GPU（gm20b）: あり", "GPU (gm20b): found") : T(@"GPU（gm20b）: なし（nvidia\gm20b）", @"GPU (gm20b): not found (nvidia\gm20b)");
            foreach (string p in f.Problems) yield return T("使えないもの: ", "Not usable: ") + p;
        }

        /// <summary>Copies the Wi-Fi and Bluetooth files into the Windows whose root is windowsRoot (C:\ or the SD's partition).</summary>
        public static void CopyToWindows(FirmwareSet f, string windowsRoot, Action<string> log)
        {
            if (f.Wifi)
            {
                string dir = Path.Combine(windowsRoot, WifiDir);
                Directory.CreateDirectory(dir);
                File.Copy(f.WifiBin, Path.Combine(dir, WifiBinName), true);
                File.Copy(f.WifiTxt, Path.Combine(dir, WifiTxtName), true);
                log(T($"Wi-Fi のファームウェアを {dir} に入れました。", $"Put the Wi-Fi firmware in {dir}."));
            }
            if (f.BtHcd != null)
            {
                string dir = Path.Combine(windowsRoot, BtDir);
                Directory.CreateDirectory(dir);
                File.Copy(f.BtHcd, Path.Combine(dir, BtName), true);
                log(T($"Bluetooth のパッチを {dir} に入れました。", $"Put the Bluetooth patch in {dir}."));
            }
            if (f.Gpu != null)
            {
                string dir = Path.Combine(windowsRoot, GpuDir);
                foreach (string n in GpuFiles)
                {
                    string to = Path.Combine(dir, n);
                    Directory.CreateDirectory(Path.GetDirectoryName(to));
                    File.Copy(Path.Combine(f.Gpu, n), to, true);
                }
                log(T($"GPU のファームウェアを {dir} に入れました（GPU キットのインストールで読み込みます）。", $"Put the GPU firmware in {dir} (the GPU kit's Install reads it)."));
            }
        }

        /// <summary>Copies the XUSB firmware to the ESP root, where the UEFI (R392+) reads it at every boot.</summary>
        public static void CopyToEsp(FirmwareSet f, string espRoot, Action<string> log)
        {
            if (f.Xusb == null) return;
            File.Copy(f.Xusb, Path.Combine(espRoot, XusbName), true);
            log(T($"USB（XUSB）のファームウェアを ESP の {XusbName} に入れました。", $"Put the USB (XUSB) firmware on the ESP as {XusbName}."));
        }

        public static bool WifiPresent(string windowsRoot) =>
            File.Exists(Path.Combine(windowsRoot, WifiDir, WifiBinName)) && File.Exists(Path.Combine(windowsRoot, WifiDir, WifiTxtName));

        public static bool BtPresent(string windowsRoot) => File.Exists(Path.Combine(windowsRoot, BtDir, BtName));

        static bool IsTestedWifi(string path)
        {
            using FileStream s = File.OpenRead(path);
            return Convert.ToHexString(SHA256.HashData(s)).Equals(TestedWifiSha256, StringComparison.OrdinalIgnoreCase);
        }

        static string Pick(string folder, string name, string alt)
        {
            foreach (string sub in SubFolders)
                foreach (string n in new[] { name, alt })
                {
                    string p = Path.Combine(folder, sub, n);
                    if (File.Exists(p)) return p;
                }
            return null;
        }

        static bool SizeOk(string path, long min, long max)
        {
            long n = new FileInfo(path).Length;
            return n >= min && n <= max;
        }

        /// <summary>An .hcd is a list of Broadcom vendor HCI commands (opcode xxFC, length, parameters) ending exactly at the end.</summary>
        static bool IsHcd(string path)
        {
            byte[] b = File.ReadAllBytes(path);
            if (b.Length < 4 || b.Length > 256 * 1024) return false;
            int off = 0;
            while (off + 3 <= b.Length)
            {
                if (b[off + 1] != 0xFC) return false;
                off += 3 + b[off + 2];
            }
            return off == b.Length;
        }
    }
}
