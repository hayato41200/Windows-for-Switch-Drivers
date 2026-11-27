// GPU drawing (experimental): switchgpu keeps the GM20B running and switchdisplay
// hands screen updates to it (kits\Switch-Gpu stages F3, F4 and G1).
//
// The NVIDIA firmware is the user's own linux-firmware nvidia\gm20b copy. SwitchPatcher
// or the firmware import puts it in C:\SwitchWOA\firmware\nvidia\gm20b; the elevated
// "--gpu on" mode copies it into Services\switchgpu\Parameters (where the driver reads
// it at boot, as Gpu.ps1 F3On does) and sets the switches. "--gpu off" clears them.
// Both take effect at the next Windows restart.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    static class GpuAccel
    {
        const string Hwid = "SWOA0F01";
        const string GpuKey = @"SYSTEM\CurrentControlSet\Services\switchgpu\Parameters";
        const string DisplayKey = @"SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters";
        // Gpu.ps1 Import-GpuFirmware: file -> registry value.
        static readonly Dictionary<string, string> FwValues = new Dictionary<string, string>
        {
            [@"acr\bl.bin"] = "FwAcrBl", [@"acr\ucode_load.bin"] = "FwAcrUcodeLoad",
            [@"pmu\desc.bin"] = "FwPmuDesc", [@"pmu\image.bin"] = "FwPmuImage", [@"pmu\sig.bin"] = "FwPmuSig",
            [@"gr\fecs_bl.bin"] = "FwFecsBl", [@"gr\fecs_inst.bin"] = "FwFecsInst", [@"gr\fecs_data.bin"] = "FwFecsData", [@"gr\fecs_sig.bin"] = "FwFecsSig",
            [@"gr\gpccs_inst.bin"] = "FwGpccsInst", [@"gr\gpccs_data.bin"] = "FwGpccsData",
            [@"gr\sw_ctx.bin"] = "FwGrSwCtx", [@"gr\sw_nonctx.bin"] = "FwGrSwNonctx", [@"gr\sw_bundle_init.bin"] = "FwGrSwBundleInit",
        };
        const int GpcKhz = 460800;          // F2c: ~5 GB/s at CVB 950 mV
        const int IdlePowerOffMs = 10000;   // G1 default

        static string FirmwareFolder => Path.Combine(Path.GetPathRoot(Environment.SystemDirectory), SwitchFirmware.GpuDir);

        /// <summary>The GPU device has the switchgpu driver (it comes with the drivers release).</summary>
        public static bool DriverInstalled()
        {
            using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\ACPI\{Hwid}");
            if (k == null) return false;
            foreach (string inst in k.GetSubKeyNames())
            {
                using RegistryKey d = k.OpenSubKey(inst);
                if (string.Equals(d?.GetValue("Service") as string, "switchgpu", StringComparison.OrdinalIgnoreCase)) return true;
            }
            return false;
        }

        static bool FolderComplete(string dir) => FwValues.Keys.All(n => File.Exists(Path.Combine(dir, n)));

        static bool RegistryComplete()
        {
            using RegistryKey k = Registry.LocalMachine.OpenSubKey(GpuKey);
            return k != null && FwValues.Values.All(v => k.GetValue(v) is byte[] b && b.Length > 0);
        }

        /// <summary>The firmware is in C:\SwitchWOA\firmware\nvidia\gm20b or already in the registry.</summary>
        public static bool FirmwarePresent() => FolderComplete(FirmwareFolder) || RegistryComplete();

        /// <summary>Both switches are on (they apply from the next boot).</summary>
        public static bool Enabled => DriverTrace.Get("switchgpu", "AutoResident") == 1 && DriverTrace.Get("switchdisplay", "GpuPresent") == 1;

        /// <summary>One line on what the GPU did since boot, or null when it is off.</summary>
        public static string StatusLine()
        {
            int? resident = DriverTrace.Get("switchgpu", "B3eResidentActive"), g1 = DriverTrace.Get("switchgpu", "G1State");
            int? presents = DriverTrace.Get("switchdisplay", "GpuPresents"), failures = DriverTrace.Get("switchdisplay", "GpuFailures");
            int? fallbacks = DriverTrace.Get("switchdisplay", "GpuFallbacks"), guard = DriverTrace.Get("switchgpu", "AutoResidentGuardTripped");
            if (guard == 1)
                return T("前回の起動で GPU の準備が終わらなかったため、GPU を止めました。オンにし直すと、もう一度試します。",
                         "The GPU did not finish starting last time, so it was turned off. Turn it on again to retry.");
            if (DriverTrace.Get("switchgpu", "AutoResident") != 1 || g1 == null) return null;
            string power = resident == 1 && g1 == 1 ? T("動作中", "running") : T("休止中（使うときに起動）", "resting (starts when needed)");
            return T($"GPU: {power}　GPU で描いた回数 {presents ?? 0}、CPU で描いた回数 {fallbacks ?? 0}、失敗 {failures ?? 0}",
                     $"GPU: {power}   drawn by GPU {presents ?? 0}, by CPU {fallbacks ?? 0}, failed {failures ?? 0}");
        }

        /// <summary>Starts this program elevated to turn GPU drawing on or off. null = the user said no to UAC.</summary>
        public static async Task<int?> RunElevatedAsync(bool on)
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath, on ? "--gpu on" : "--gpu off") { UseShellExecute = true, Verb = "runas" };
            Process p;
            try { p = Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return null; }
            using (p)
            {
                await p.WaitForExitAsync();
                return p.ExitCode;
            }
        }

        /// <summary>The elevated part. Returns 0 when the switches were written.</summary>
        public static int Apply(bool on)
        {
            var log = new List<string>();
            int code = 1;
            try
            {
                if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                    throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));
                using RegistryKey gpu = Registry.LocalMachine.CreateSubKey(GpuKey);
                using RegistryKey display = Registry.LocalMachine.CreateSubKey(DisplayKey);
                if (on)
                {
                    if (!DriverInstalled())
                        throw new InvalidOperationException(T("GPU のドライバー（switchgpu）が入っていません。「ドライバー」タブから入れてください。",
                                                              "The GPU driver (switchgpu) is not installed. Install it from the Drivers tab."));
                    if (FolderComplete(FirmwareFolder))
                    {
                        foreach (var f in FwValues)
                            gpu.SetValue(f.Value, File.ReadAllBytes(Path.Combine(FirmwareFolder, f.Key)), RegistryValueKind.Binary);
                        log.Add(T($"GPU のファームウェアを {FirmwareFolder} から読み込みました。", $"Read the GPU firmware from {FirmwareFolder}."));
                    }
                    else if (RegistryComplete())
                        log.Add(T("前に読み込んだ GPU のファームウェアを使います。", "Using the GPU firmware read earlier."));
                    else
                        throw new FileNotFoundException(T($"GPU のファームウェアがありません。自分の linux-firmware の nvidia\\gm20b を、「ドライバー」タブのファームウェアの取り込みか SwitchPatcher で入れてください（{FirmwareFolder}）。",
                                                          $"The GPU firmware is missing. Add your own linux-firmware nvidia\\gm20b with the firmware import on the Drivers tab or with SwitchPatcher ({FirmwareFolder})."));
                    if (gpu.GetValue("IdlePowerOffMs") == null) gpu.SetValue("IdlePowerOffMs", IdlePowerOffMs, RegistryValueKind.DWord);
                }
                int v = on ? 1 : 0;
                gpu.SetValue("AutoResident", v, RegistryValueKind.DWord);
                gpu.SetValue("GpcKhz", on ? GpcKhz : 0, RegistryValueKind.DWord);
                gpu.SetValue("AutoResidentPending", 0, RegistryValueKind.DWord);
                gpu.SetValue("AutoResidentGuardTripped", 0, RegistryValueKind.DWord);
                display.SetValue("GpuPresent", v, RegistryValueKind.DWord);
                display.SetValue("DoubleBuffer", v, RegistryValueKind.DWord);
                log.Add(on ? T("GPU での描画をオンにしました。", "Turned GPU drawing on.") : T("GPU での描画をオフにしました。", "Turned GPU drawing off."));
                log.Add(T("Windows を再起動すると切り替わります。", "It takes effect when Windows restarts."));
                code = 0;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidOperationException || e is System.Security.SecurityException)
            {
                log.Add(T("失敗しました: ", "Failed: ") + e.Message);
            }
            MessageBox.Show(string.Join(Environment.NewLine, log), T("GPU", "GPU"),
                            MessageBoxButtons.OK, code == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Error);
            return code;
        }
    }
}
