// Changes to a Windows that is not running: its SOFTWARE registry hive
// (loaded with reg.exe under HKLM\SWOA_PATCH_SOFTWARE), certificates written
// straight into its certificate stores, and the boot store on the ESP.
using System;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    static class Offline
    {
        const string MountName = "SWOA_PATCH_SOFTWARE";

        /// <summary>Loads the offline SOFTWARE hive, runs body on it, and always unloads it.</summary>
        public static async Task WithSoftwareHive(string windows, Action<RegistryKey> body, Action<string> log)
        {
            string hive = Path.Combine(windows, @"Windows\System32\config\SOFTWARE");
            if (!File.Exists(hive)) throw new FileNotFoundException(hive);
            if (await Tools.Run(Tools.System32("reg.exe"), $"load HKLM\\{MountName} \"{hive}\"", log) != 0)
                throw new InvalidOperationException(T("レジストリ（SOFTWARE）を読み込めませんでした。", "Could not load the SOFTWARE hive."));
            try
            {
                using RegistryKey k = Registry.LocalMachine.OpenSubKey(MountName, true);
                body(k);
            }
            finally
            {
                // Every handle into the hive must be closed before reg unload succeeds.
                for (int attempt = 0; ; attempt++)
                {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                    if (await Tools.Run(Tools.System32("reg.exe"), $"unload HKLM\\{MountName}", log) == 0) break;
                    if (attempt == 4) throw new InvalidOperationException(T("レジストリ（SOFTWARE）を閉じられませんでした。PC を再起動してから SD を外してください。",
                                                                           "Could not unload the SOFTWARE hive. Restart the PC before removing the SD."));
                    Thread.Sleep(1000);
                }
            }
        }

        /// <summary>
        /// The registry "Blob" Windows keeps for a certificate in a store. Made by
        /// adding the certificate to a temporary store of this user and reading
        /// it back, so the format is exactly Windows' own.
        /// </summary>
        public static byte[] CertificateBlob(X509Certificate2 cert)
        {
            const string temp = "SwitchPatcherTemp";
            using (var store = new X509Store(temp, StoreLocation.CurrentUser))
            {
                store.Open(OpenFlags.ReadWrite);
                store.Add(cert);
            }
            try
            {
                using RegistryKey k = Registry.CurrentUser.OpenSubKey($@"Software\Microsoft\SystemCertificates\{temp}\Certificates\{cert.Thumbprint}");
                return k?.GetValue("Blob") as byte[] ?? throw new InvalidOperationException("certificate blob not found");
            }
            finally
            {
                Registry.CurrentUser.DeleteSubKeyTree($@"Software\Microsoft\SystemCertificates\{temp}", false);
            }
        }

        /// <summary>Puts the certificate into the offline machine store (ROOT, TrustedPublisher, ...).</summary>
        public static void AddCertificate(RegistryKey software, string store, string thumbprint, byte[] blob)
        {
            using RegistryKey k = software.CreateSubKey($@"Microsoft\SystemCertificates\{store}\Certificates\{thumbprint}");
            k.SetValue("Blob", blob, RegistryValueKind.Binary);
        }

        /// <summary>Starts a program at every login (HKLM Run).</summary>
        public static void AddRun(RegistryKey software, string name, string command)
        {
            using RegistryKey k = software.CreateSubKey(@"Microsoft\Windows\CurrentVersion\Run");
            k.SetValue(name, command, RegistryValueKind.String);
        }

        /// <summary>Lets the first-boot setup continue without a network (the Switch has no Wi-Fi driver yet).</summary>
        public static void AllowOobeWithoutNetwork(RegistryKey software)
        {
            using RegistryKey k = software.CreateSubKey(@"Microsoft\Windows\CurrentVersion\OOBE");
            k.SetValue("BypassNRO", 1, RegistryValueKind.DWord);
        }

        /// <summary>
        /// First-boot answers: skip the Microsoft account and Wi-Fi pages. No
        /// account or password is set here; the user creates a local account in
        /// the setup screens.
        /// </summary>
        public static void WriteUnattend(string windows)
        {
            string dir = Path.Combine(windows, @"Windows\Panther");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "unattend.xml"), @"<?xml version=""1.0"" encoding=""utf-8""?>
<unattend xmlns=""urn:schemas-microsoft-com:unattend"">
  <settings pass=""oobeSystem"">
    <component name=""Microsoft-Windows-Shell-Setup"" processorArchitecture=""arm64"" publicKeyToken=""31bf3856ad364e35"" language=""neutral"" versionScope=""nonSxS"" xmlns:wcm=""http://schemas.microsoft.com/WMIConfig/2002/State"">
      <OOBE>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
    </component>
  </settings>
</unattend>
");
        }

        /// <summary>Copies the ESP's Windows boot files to the PC before they are changed. Returns the backup folder.</summary>
        public static string BackupEsp(string esp)
        {
            string backup = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "SwitchPatcher",
                                         "backup-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            foreach (string sub in new[] { @"EFI\Microsoft\Boot", @"EFI\Boot" })
            {
                string src = Path.Combine(esp, sub);
                if (!Directory.Exists(src)) continue;
                foreach (string f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                {
                    string dest = Path.Combine(backup, sub, Path.GetRelativePath(src, f));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    File.Copy(f, dest, true);
                }
            }
            Directory.CreateDirectory(backup);
            return backup;
        }
    }
}
