// OTA driver updates from the GitHub releases of Windows-for-Switch-Drivers
// (design: docs\Design-DriverRepo-OTA-ja.md). The window only checks and
// compares, as the normal user. Installing restarts SwitchCenter elevated
// with --install-drivers <tag> <services>; that process fetches the same
// release again by itself, into a folder only administrators can write, and
// checks the ZIP's SHA-256, each .sys against the manifest and each .cat's
// signer before it runs pnputil. Nothing from the unelevated process is trusted.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Security.Principal;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    sealed class ManifestDriver
    {
        public string Service, Inf, Hwid, Version, MinUefi, SysSha256;
    }

    sealed class Release
    {
        public string Tag, PageUrl, AppVersion, DriversFile, DriversSha256, DriversUrl;
        public List<ManifestDriver> Drivers = new List<ManifestDriver>();
    }

    /// <summary>One driver of the release compared with this Switch.</summary>
    sealed class UpdateRow
    {
        public ManifestDriver Item;
        public string Installed;          // lowest installed version over the device's instances, null = no driver
        public bool DevicePresent, UefiTooOld, Update;
    }

    static class Updater
    {
        public const string Repo = "hayato41200/Windows-for-Switch-Drivers";
        /// <summary>Thumbprint of "CN=SwitchWOA Test Driver Signing"; every .cat must be signed with it.</summary>
        public const string Signer = "A9E1F054DFCC96F43010918892C3C30D5A522712";
        static readonly Regex TagPattern = new Regex(@"^drivers-\d{4}\.\d{1,2}\.\d{1,2}(\.\d+)?$");
        static readonly Regex ServicePattern = new Regex(@"^[a-z0-9]{1,32}$");

        static HttpClient NewClient()
        {
            var c = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
            c.DefaultRequestHeaders.UserAgent.ParseAdd("SwitchCenter/" + Application.ProductVersion);
            return c;
        }

        /// <summary>Reads releases/latest (tag == null) or the given tag, and its manifest.json.</summary>
        public static async Task<Release> FetchAsync(string tag)
        {
            if (tag != null && !TagPattern.IsMatch(tag)) throw new InvalidDataException("bad tag " + tag);
            using HttpClient http = NewClient();
            string api = $"https://api.github.com/repos/{Repo}/releases/" + (tag == null ? "latest" : "tags/" + tag);
            using JsonDocument rel = JsonDocument.Parse(await http.GetStringAsync(api));
            var r = new Release
            {
                Tag = rel.RootElement.GetProperty("tag_name").GetString(),
                PageUrl = rel.RootElement.GetProperty("html_url").GetString(),
            };
            if (!TagPattern.IsMatch(r.Tag) || (tag != null && r.Tag != tag)) throw new InvalidDataException("unexpected tag " + r.Tag);
            var assets = new Dictionary<string, string>();
            foreach (JsonElement a in rel.RootElement.GetProperty("assets").EnumerateArray())
            {
                string url = a.GetProperty("browser_download_url").GetString();
                // Only files attached to a release of this repository, over HTTPS.
                if (url.StartsWith($"https://github.com/{Repo}/releases/download/", StringComparison.Ordinal))
                    assets[a.GetProperty("name").GetString()] = url;
            }
            if (!assets.TryGetValue("manifest.json", out string manifestUrl)) throw new InvalidDataException("manifest.json is missing in " + r.Tag);

            using JsonDocument m = JsonDocument.Parse(await http.GetStringAsync(manifestUrl));
            JsonElement root = m.RootElement;
            if (root.GetProperty("release").GetString() != r.Tag) throw new InvalidDataException("manifest.json is for another release");
            r.AppVersion = root.GetProperty("app").GetProperty("version").GetString();
            JsonElement drv = root.GetProperty("drivers");
            r.DriversFile = drv.GetProperty("file").GetString();
            r.DriversSha256 = drv.GetProperty("sha256").GetString().ToLowerInvariant();
            if (!assets.TryGetValue(r.DriversFile, out r.DriversUrl)) throw new InvalidDataException(r.DriversFile + " is missing in " + r.Tag);
            foreach (JsonElement i in drv.GetProperty("items").EnumerateArray())
            {
                var d = new ManifestDriver
                {
                    Service = i.GetProperty("service").GetString(),
                    Inf = i.GetProperty("inf").GetString(),
                    Hwid = i.GetProperty("hwid").GetString(),
                    Version = i.GetProperty("version").GetString(),
                    MinUefi = i.GetProperty("minUefi").GetString(),
                    SysSha256 = i.GetProperty("sysSha256").GetString().ToLowerInvariant(),
                };
                if (!ServicePattern.IsMatch(d.Service) || !System.Version.TryParse(d.Version, out _)) throw new InvalidDataException("bad manifest entry " + d.Service);
                r.Drivers.Add(d);
            }
            return r;
        }

        // ------------------------------------------------------------ compare

        /// <summary>
        /// The UEFI has no version string of its own (SMBIOS only carries the
        /// EDK2 commit), so estimate it from the newest Switch device in the DSDT:
        /// each R3xx below added one. Returns e.g. 377, or 0 when none is present.
        /// </summary>
        public static int EstimateUefi()
        {
            (string Hwid, int Rev)[] added =
            {
                ("SWOA0801", 377), ("SWOA0701", 375), ("SWOA0601", 374), ("SWOA0501", 373),
                ("SWOA0401", 372), ("SWOA0301", 371), ("SWOA0201", 370), ("SWOA0101", 367),
            };
            foreach (var (hwid, rev) in added)
            {
                using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\ACPI\{hwid}");
                if (k != null && k.SubKeyCount > 0) return rev;
            }
            return 0;
        }

        static int UefiNumber(string r) => r != null && r.Length > 1 && int.TryParse(r.Substring(1), out int n) ? n : 0;

        public static List<UpdateRow> Compare(Release release, List<DriverRow> installed, int uefi)
        {
            var rows = new List<UpdateRow>();
            foreach (ManifestDriver d in release.Drivers)
            {
                var row = new UpdateRow { Item = d, UefiTooOld = UefiNumber(d.MinUefi) > uefi };
                Version lowest = null;
                bool missingDriver = false;
                foreach (DriverRow i in installed.Where(i => i.HardwareId.Equals(d.Hwid, StringComparison.OrdinalIgnoreCase) && i.Instance != ""))
                {
                    row.DevicePresent = true;
                    if (System.Version.TryParse(i.Installed, out Version v)) { if (lowest == null || v < lowest) lowest = v; }
                    else missingDriver = true;
                }
                row.Installed = missingDriver ? null : lowest?.ToString();
                row.Update = row.DevicePresent && !row.UefiTooOld && (missingDriver || lowest == null || lowest < System.Version.Parse(d.Version));
                rows.Add(row);
            }
            return rows;
        }

        public static bool AppIsOlder(Release release) =>
            System.Version.TryParse(release.AppVersion, out Version latest) &&
            System.Version.TryParse(Application.ProductVersion.Split('+')[0], out Version mine) && mine < latest;

        /// <summary>Starts the elevated installer and waits for it. Returns its exit code, or null if UAC was declined.</summary>
        public static async Task<int?> RunElevatedAsync(string tag, IEnumerable<string> services)
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath, $"--install-drivers {tag} {string.Join(",", services)}")
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            Process p;
            try { p = Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return null; }   // ERROR_CANCELLED
            using (p)
            {
                await p.WaitForExitAsync();
                return p.ExitCode;
            }
        }

        // ------------------------------------------------- elevated installer

        public const int ExitOk = 0, ExitReboot = 3010, ExitFailed = 1;

        /// <summary>Runs in the elevated process. log() shows progress; returns the exit code.</summary>
        public static async Task<int> InstallAsync(string tag, string[] services, Action<string> log)
        {
            if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));

            log(T($"リリース {tag} を GitHub から読んでいます…", $"Reading release {tag} from GitHub..."));
            Release release = await FetchAsync(tag);
            int uefi = EstimateUefi();
            var chosen = new List<ManifestDriver>();
            foreach (ManifestDriver d in release.Drivers)               // manifest order = install order (GPIO, I2C, ...)
            {
                if (Array.IndexOf(services, d.Service) < 0) continue;
                if (UefiNumber(d.MinUefi) > uefi)
                    throw new InvalidOperationException(T($"{d.Service} には UEFI {d.MinUefi} 以降が必要です（今は R{uefi} 相当）。",
                                                          $"{d.Service} needs UEFI {d.MinUefi} or later (this one looks like R{uefi})."));
                chosen.Add(d);
            }
            if (chosen.Count == 0) throw new InvalidOperationException(T("入れるドライバーがありません。", "Nothing to install."));

            string work = CreateAdminOnlyFolder();
            try
            {
                string zip = Path.Combine(work, "drivers.zip");
                log(T($"{release.DriversFile} をダウンロードしています…", $"Downloading {release.DriversFile}..."));
                using (HttpClient http = NewClient())
                using (Stream src = await http.GetStreamAsync(release.DriversUrl))
                using (FileStream dst = File.Create(zip))
                    await src.CopyToAsync(dst);
                string hash = Sha256(zip);
                if (hash != release.DriversSha256)
                    throw new InvalidDataException(T($"ZIP の SHA-256 が manifest と合いません（{hash}）。", $"The ZIP's SHA-256 does not match the manifest ({hash})."));
                log(T("SHA-256 が manifest と合いました。", "SHA-256 matches the manifest."));

                string files = Path.Combine(work, "files");
                ZipFile.ExtractToDirectory(zip, files);                  // rejects entries outside the folder

                foreach (ManifestDriver d in chosen)
                {
                    string inf = Path.GetFullPath(Path.Combine(files, d.Inf));
                    if (!inf.StartsWith(files + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(inf))
                        throw new InvalidDataException(d.Inf + T(" がありません。", " is missing."));
                    string dir = Path.GetDirectoryName(inf);
                    string sys = Path.Combine(dir, d.Service + ".sys"), cat = Path.Combine(dir, d.Service + ".cat");
                    if (!File.Exists(sys) || Sha256(sys) != d.SysSha256)
                        throw new InvalidDataException(T($"{d.Service}.sys が manifest と合いません。", $"{d.Service}.sys does not match the manifest."));
                    CheckCatalogSigner(cat);
                    log(T($"{d.Service} {d.Version}: .sys と署名を確かめました。", $"{d.Service} {d.Version}: .sys and signature checked."));
                }

                bool reboot = false;
                string pnputil = Path.Combine(Environment.SystemDirectory, "pnputil.exe");
                foreach (ManifestDriver d in chosen)
                {
                    log(T($"{d.Service} を入れています…", $"Installing {d.Service}..."));
                    var psi = new ProcessStartInfo(pnputil, $"/add-driver \"{Path.Combine(files, d.Inf)}\" /install")
                    {
                        UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true,
                    };
                    using Process p = Process.Start(psi);
                    string output = await p.StandardOutput.ReadToEndAsync();
                    await p.WaitForExitAsync();
                    foreach (string line in output.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0)) log("  " + line);
                    if (p.ExitCode == ExitReboot) reboot = true;
                    else if (p.ExitCode != 0)
                        throw new InvalidOperationException(T($"pnputil が失敗しました（{p.ExitCode}）。", $"pnputil failed ({p.ExitCode})."));
                }
                return reboot ? ExitReboot : ExitOk;
            }
            finally
            {
                try { Directory.Delete(work, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }

        static string Sha256(string path)
        {
            using FileStream f = File.OpenRead(path);
            return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
        }

        /// <summary>The catalog must be a valid PKCS #7 signature by the project's test certificate.</summary>
        static void CheckCatalogSigner(string cat)
        {
            if (!File.Exists(cat)) throw new InvalidDataException(Path.GetFileName(cat) + T(" がありません。", " is missing."));
            var cms = new SignedCms();
            cms.Decode(File.ReadAllBytes(cat));
            cms.CheckSignature(true);                                    // the signature itself; trust is Windows' job
            if (cms.SignerInfos.Count != 1 || !string.Equals(cms.SignerInfos[0].Certificate?.Thumbprint, Signer, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(T($"{Path.GetFileName(cat)} の署名が、このプロジェクトの証明書ではありません。",
                                                 $"{Path.GetFileName(cat)} is not signed with this project's certificate."));
        }

        /// <summary>A new folder under %windir%\Temp that only SYSTEM and Administrators can access.</summary>
        static string CreateAdminOnlyFolder()
        {
            var sec = new DirectorySecurity();
            sec.SetAccessRuleProtection(true, false);
            foreach (WellKnownSidType sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
                    InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Temp", "SwitchCenter-OTA-" + Guid.NewGuid().ToString("N"));
            sec.CreateDirectory(path);
            return path;
        }
    }

    /// <summary>The elevated process's window: progress log, then the result.</summary>
    sealed class InstallForm : Form
    {
        readonly TextBox logBox = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical };
        readonly Button close = new Button { Dock = DockStyle.Bottom, Height = 48, Enabled = false };
        public int Result { get; private set; } = Updater.ExitFailed;

        public InstallForm(string tag, string[] services)
        {
            Text = T("ドライバーの更新", "Driver update");
            Font = new System.Drawing.Font("Yu Gothic UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new System.Drawing.Size(560, 520);
            close.Text = T("閉じる", "Close");
            close.Click += (_, _) => Close();
            Controls.Add(logBox);
            Controls.Add(close);
            FormClosing += (_, e) => { if (!close.Enabled) e.Cancel = true; };   // not while pnputil runs
            Shown += async (_, _) =>
            {
                try
                {
                    Result = await Updater.InstallAsync(tag, services, Log);
                    Log("");
                    Log(Result == Updater.ExitReboot ? T("完了しました。再起動すると新しいドライバーになります。", "Done. Restart to finish.")
                                                     : T("完了しました。", "Done."));
                }
                catch (Exception e)
                {
                    Result = Updater.ExitFailed;
                    Log("");
                    Log(T("中止しました: ", "Stopped: ") + e.Message);
                }
                close.Enabled = true;
            };
        }

        void Log(string line) => logBox.AppendText(line + Environment.NewLine);
    }
}
