// The work itself, in order. Each step logs what it does; any failure stops
// the run (the steps already done stay done, and the ESP backup is kept).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography.X509Certificates;
using System.Threading.Tasks;
using SwitchCenter;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    sealed class PatchOptions
    {
        public DiskInfo Disk;
        public PartInfo Windows;
        public bool InitializeDisk;       // erase the whole SD and make GPT + ESP + NTFS first (implies InstallImage)
        public bool InstallImage;         // format the Windows partition and apply ImagePath / Index first
        public string ImagePath;          // .iso, .wim or .esd
        public int ImageIndex;
        public bool Drivers, TestMode, ControlCenter;
    }

    static class Patcher
    {
        /// <summary>Where the control center goes on the Switch: a folder normal users can write, so it can update itself.</summary>
        const string AppFolder = @"SwitchWOA\SwitchCenter";

        public static async Task RunAsync(PatchOptions o, Action<string> log, Action<string> progress)
        {
            var assigned = new List<(int Disk, int Part, string Letter)>();
            string work = null, mountedIso = null;
            try
            {
                await Disks.CheckNotSystemDisk(o.Disk.Number);

                // ---- release: download and check everything before touching the SD
                log(T("GitHub の最新のリリースを読んでいます…", "Reading the latest release from GitHub..."));
                Release rel = await ReleaseClient.FetchAsync(null);
                log(T($"リリース: {rel.Tag}（コントロールセンター {rel.AppVersion}）", $"Release: {rel.Tag} (control center {rel.AppVersion})"));
                work = ReleaseClient.CreateAdminOnlyFolder();
                string files = Path.Combine(work, "drivers");
                var infs = new List<string>();
                X509Certificate2 cert = null;
                if (o.Drivers || o.TestMode)
                {
                    string zip = Path.Combine(work, rel.DriversFile);
                    log(T($"{rel.DriversFile} をダウンロードしています…", $"Downloading {rel.DriversFile}..."));
                    await ReleaseClient.DownloadAsync(rel.DriversUrl, zip, rel.DriversSha256);
                    ZipFile.ExtractToDirectory(zip, files);
                    foreach (ManifestDriver d in rel.Drivers)
                    {
                        infs.Add(ReleaseClient.VerifyDriver(files, d));
                        log(T($"  {d.Service} {d.Version}: .sys と署名を確かめました。", $"  {d.Service} {d.Version}: .sys and signature checked."));
                    }
                    cert = new X509Certificate2(Path.Combine(files, "SwitchWOA-Test.cer"));
                    if (!string.Equals(cert.Thumbprint, ReleaseClient.Signer, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException(T("リリースの証明書が、このプロジェクトのものではありません。", "The release's certificate is not this project's."));
                }
                string appSource = null;
                if (o.ControlCenter)
                {
                    if (rel.AppUrl == null) throw new InvalidDataException(rel.AppFile + T(" がリリースにありません。", " is missing in the release."));
                    string zip = Path.Combine(work, rel.AppFile);
                    log(T($"{rel.AppFile} をダウンロードしています…", $"Downloading {rel.AppFile}..."));
                    await ReleaseClient.DownloadAsync(rel.AppUrl, zip, rel.AppSha256);
                    ZipFile.ExtractToDirectory(zip, Path.Combine(work, "app"));
                    appSource = Path.Combine(work, "app", "SwitchCenter");
                    if (!File.Exists(Path.Combine(appSource, "SwitchCenter.exe"))) throw new InvalidDataException("SwitchCenter.exe");
                }
                log(T("ダウンロードしたものは、すべて確かめました。", "Everything downloaded has been checked."));

                // ---- a new SD: erase it and make the partitions, then read its layout again
                if (o.InitializeDisk)
                {
                    await Disks.InitializeSd(o.Disk, log);
                    o.Disk = (await Disks.ListAsync()).Find(x => x.Number == o.Disk.Number)
                             ?? throw new InvalidOperationException(T("初期化したディスクが見つかりません。", "The initialized disk has gone."));
                    o.Windows = o.Disk.Partitions.Where(p => !p.IsEsp && p.FileSystem == "NTFS").OrderByDescending(p => p.Size).First();
                    o.InstallImage = true;
                }

                PartInfo espPart = o.Disk.Esp ?? throw new InvalidOperationException(T("この SD に ESP（EFI システム区画）がありません。", "This SD has no EFI system partition."));
                string win = await Disks.EnsureLetter(o.Disk.Number, o.Windows, assigned) + @":\";
                string esp = await Disks.EnsureLetter(o.Disk.Number, espPart, assigned) + @":\";
                log(T($"Windows の区画: {win}  ESP: {esp}", $"Windows partition: {win}  ESP: {esp}"));

                // ---- Windows image
                string backup = null;
                if (o.InstallImage)
                {
                    string image = o.ImagePath;
                    if (image.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
                    {
                        string iso = await Images.MountIso(image);
                        mountedIso = image;
                        image = Images.FindInstallImage(iso + @":\");
                    }
                    log(T($"Windows の区画（{win}）をフォーマットしています…", $"Formatting the Windows partition ({win})..."));
                    await Disks.FormatWindowsPartition(o.Disk.Number, o.Windows, win.Substring(0, 1));
                    log(T("Windows を展開しています（数分〜数十分かかります）…", "Applying Windows (this takes minutes)..."));
                    if (await Tools.Run(Tools.System32("dism.exe"), $"/Apply-Image /ImageFile:\"{image}\" /Index:{o.ImageIndex} /ApplyDir:{win}", log, progress) != 0)
                        throw new InvalidOperationException(T("dism /Apply-Image が失敗しました。", "dism /Apply-Image failed."));
                    backup = Offline.BackupEsp(esp);
                    log(T($"ESP の起動ファイルを保存しました: {backup}", $"Saved the ESP boot files: {backup}"));
                    if (await Tools.Run(Tools.System32("bcdboot.exe"), $"{win}Windows /s {esp.TrimEnd('\\')} /f UEFI /v", log) != 0)
                    {
                        log(T("bcdboot が失敗したので、起動ファイルのコピーと BCD の作成を自分で行います…", "bcdboot failed; copying the boot files and building the BCD directly..."));
                        await Offline.MakeBootFiles(win, esp, log);
                        log(T("起動ファイルと BCD を作りました。", "Created the boot files and the BCD."));
                    }
                    Offline.WriteUnattend(win);
                    log(T("初回起動の設定（unattend.xml）を置きました。", "Wrote the first-boot answers (unattend.xml)."));
                }
                else if (!File.Exists(Path.Combine(win, @"Windows\System32\config\SOFTWARE")))
                {
                    throw new InvalidOperationException(T($"{win} に Windows がありません。", $"There is no Windows on {win}."));
                }

                // ---- offline registry: the SD drivers load at every boot (needed to boot from the Switch's SD)
                await Offline.WithHive(win, "SYSTEM", system =>
                {
                    foreach (string line in Offline.LoadSdDriversAtBoot(system)) log("  " + line);
                }, log);
                log(T("SD のドライバー（sdbus、sdstor）を、起動のときに必ず読み込むようにしました。", "The SD drivers (sdbus, sdstor) now load at every boot."));

                // ---- offline registry: certificate, autostart, first boot without network
                if (o.TestMode || o.ControlCenter || o.InstallImage)
                {
                    byte[] blob = cert != null ? Offline.CertificateBlob(cert) : null;
                    await Offline.WithSoftwareHive(win, soft =>
                    {
                        if (o.TestMode && blob != null)
                        {
                            Offline.AddCertificate(soft, "ROOT", cert.Thumbprint, blob);
                            Offline.AddCertificate(soft, "TrustedPublisher", cert.Thumbprint, blob);
                            log(T("テスト証明書を、信頼されたルート証明機関と信頼された発行元に入れました。", "Added the test certificate to Trusted Root and Trusted Publishers."));
                        }
                        if (o.ControlCenter)
                        {
                            Offline.AddRun(soft, "SwitchCenter", "\"C:\\" + AppFolder + "\\SwitchCenter.exe\"");
                            log(T("ログインしたときにコントロールセンターを起動するようにしました。", "The control center starts at every login."));
                        }
                        if (o.InstallImage)
                        {
                            Offline.AllowOobeWithoutNetwork(soft);
                            log(T("初回起動を、ネットワーク無しで進められるようにしました。", "First-boot setup can continue without a network."));
                        }
                    }, log);
                }

                // ---- drivers
                if (o.Drivers)
                {
                    foreach (string inf in infs)
                    {
                        int code = await Tools.Run(Tools.System32("dism.exe"), $"/Image:{win} /Add-Driver /Driver:\"{inf}\"", log, progress);
                        if (code != 0)
                            code = await Tools.Run(Tools.System32("dism.exe"), $"/Image:{win} /Add-Driver /Driver:\"{inf}\" /ForceUnsigned", log, progress);
                        if (code != 0) throw new InvalidOperationException(T($"{Path.GetFileName(inf)} を入れられませんでした。", $"Could not add {Path.GetFileName(inf)}."));
                    }
                    log(T($"ドライバーを {infs.Count} 個入れました。", $"Added {infs.Count} drivers."));
                }

                // ---- test signing in the boot store
                if (o.TestMode)
                {
                    string bcd = Path.Combine(esp, @"EFI\Microsoft\Boot\BCD");
                    if (!File.Exists(bcd)) throw new FileNotFoundException(bcd);
                    if (backup == null)
                    {
                        backup = Offline.BackupEsp(esp);
                        log(T($"ESP の起動ファイルを保存しました: {backup}", $"Saved the ESP boot files: {backup}"));
                    }
                    if (await Tools.Run(Tools.System32("bcdedit.exe"), $"/store \"{bcd}\" /set {{default}} testsigning on", log) != 0)
                        throw new InvalidOperationException(T("bcdedit が失敗しました。", "bcdedit failed."));
                    await Tools.Run(Tools.System32("bcdedit.exe"), $"/store \"{bcd}\" /enum {{default}}", log);
                }

                // ---- control center
                if (o.ControlCenter)
                {
                    string dest = Path.Combine(win, AppFolder);
                    foreach (string f in Directory.GetFiles(appSource, "*", SearchOption.AllDirectories))
                    {
                        string to = Path.Combine(dest, Path.GetRelativePath(appSource, f));
                        Directory.CreateDirectory(Path.GetDirectoryName(to));
                        File.Copy(f, to, true);
                    }
                    log(T($"コントロールセンター {rel.AppVersion} を C:\\{AppFolder} に置きました。", $"Put control center {rel.AppVersion} in C:\\{AppFolder}."));
                }

                log("");
                log(T("完了しました。SD を「安全に取り外す」から外して、Switch で起動してください。", "Done. Eject the SD safely and boot it on the Switch."));
            }
            finally
            {
                foreach (var (disk, part, letter) in assigned) await Disks.RemoveLetter(disk, part, letter);
                if (mountedIso != null) await Images.DismountIso(mountedIso);
                if (work != null)
                    try { Directory.Delete(work, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }
    }
}
