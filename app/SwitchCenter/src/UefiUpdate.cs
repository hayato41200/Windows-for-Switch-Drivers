// UEFI update for the hekate boot chain (hekate -> bl31 -> bl33.bin).
//
// The UEFI built for hekate is a file on the SD's ESP: switchroot\switchwoa\bl33.bin.
// A release can carry one (manifest "uefi", bl33-<version>.bin, SHA-256 in the
// manifest). "--update-uefi <tag>" runs elevated: it fetches the release itself,
// checks the file's SHA-256 and that it looks like the Switch UEFI (_FVH at
// 0x1028), mounts the ESP (mountvol /s), and only if bl33.bin is already there
// (= this Switch boots Windows from hekate) replaces it.
//
// Before replacing, the running UEFI is kept as a second hekate entry: the old
// bl33.bin and bl31.bin go to switchroot\switchwoa-prev\ and hekate_ipl.ini gets
// [Windows (previous UEFI)] once. If the new UEFI does not boot, choosing that
// entry in hekate starts the old one, without a PC.
//
// The RCM (coreboot) chain is not updated here: its ROM lives on the PC that
// sends it, and it is not distributed.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    static class UefiUpdate
    {
        public const int ExitUpdated = 0, ExitFailed = 1, ExitUpToDate = 2, ExitNotHekate = 4;
        const string Dir = @"switchroot\switchwoa", PrevDir = @"switchroot\switchwoa-prev";
        const string PrevEntry = "[Windows (previous UEFI)]\r\nl4t=1\r\nboot_prefixes=switchroot/switchwoa-prev/\r\nid=SWOAPRV\r\n";

        /// <summary>
        /// The running UEFI's version from SMBIOS (BIOS version "1.09 R442H (commit)", from R442 on):
        /// "R442H" = hekate build, "R442" = RCM (coreboot) build. null for older UEFIs, which carry only the commit.
        /// </summary>
        public static string RunningVersion()
        {
            using Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS");
            Match m = Regex.Match(k?.GetValue("BIOSVersion") as string ?? "", @"\b(R\d{3,4}H?)\b");
            return m.Success ? m.Groups[1].Value : null;
        }

        public static int Number(string version) =>
            version != null && Regex.Match(version, @"^R(\d{3,4})") is { Success: true } m ? int.Parse(m.Groups[1].Value) : 0;

        public static bool IsHekate(string version) => version != null && version.EndsWith("H");

        /// <summary>Starts this program elevated to update bl33.bin from the release tag. null = UAC declined.</summary>
        public static async Task<int?> RunElevatedAsync(string tag)
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath, $"--update-uefi {tag}") { UseShellExecute = true, Verb = "runas" };
            Process p;
            try { p = Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return null; }
            using (p)
            {
                await p.WaitForExitAsync();
                return p.ExitCode;
            }
        }

        /// <summary>bl33.bin must look like this project's hekate build: the stub, then the UEFI firmware volume at 0x1000.</summary>
        public static void CheckBl33(string path)
        {
            var info = new FileInfo(path);
            if (info.Length < 0x40000 || info.Length > 0x1000000) throw new InvalidDataException(T($"bl33.bin の大きさがおかしいです（{info.Length} バイト）。", $"bl33.bin has an odd size ({info.Length} bytes)."));
            byte[] head = new byte[0x1030];
            using (FileStream f = File.OpenRead(path)) f.ReadExactly(head);
            if (Encoding.ASCII.GetString(head, 0x1028, 4) != "_FVH") throw new InvalidDataException(T("bl33.bin が Switch の UEFI の形ではありません。", "bl33.bin does not look like the Switch UEFI."));
        }

        /// <summary>The elevated part. log() shows progress; returns one of the Exit codes.</summary>
        public static async Task<int> RunAsync(string tag, Action<string> log)
        {
            if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));

            log(T($"リリース {tag} を GitHub から読んでいます…", $"Reading release {tag} from GitHub..."));
            Release release = await ReleaseClient.FetchAsync(tag);
            if (release.Uefi == null) throw new InvalidDataException(T($"{tag} には UEFI（bl33）がありません。", $"{tag} has no UEFI (bl33)."));
            string work = ReleaseClient.CreateAdminOnlyFolder();
            string letter = null;
            try
            {
                string bl33 = Path.Combine(work, release.Uefi.Bl33File);
                log(T($"{release.Uefi.Bl33File}（UEFI {release.Uefi.Version}）をダウンロードしています…", $"Downloading {release.Uefi.Bl33File} (UEFI {release.Uefi.Version})..."));
                await ReleaseClient.DownloadAsync(release.Uefi.Bl33Url, bl33, release.Uefi.Bl33Sha256);
                CheckBl33(bl33);
                log(T("SHA-256 が manifest と合いました。", "SHA-256 matches the manifest."));

                letter = MountEsp(log);
                return ApplyToEsp(letter + @":\", bl33, release.Uefi.Version, release.Uefi.Bl33Sha256, log);
            }
            finally
            {
                if (letter != null) UnmountEsp(letter, log);
                try { Directory.Delete(work, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }

        /// <summary>Replaces bl33.bin on the mounted ESP with the checked file, keeping the old one as the previous entry.</summary>
        internal static int ApplyToEsp(string esp, string bl33, string version, string sha, Action<string> log)
        {
            string cur = Path.Combine(esp, Dir, "bl33.bin");
            if (!File.Exists(cur))
            {
                log(T("ESP に switchroot\\switchwoa\\bl33.bin がありません。この Switch は hekate から Windows を起動していないようです。",
                      "There is no switchroot\\switchwoa\\bl33.bin on the ESP: this Switch does not seem to boot Windows from hekate."));
                log(T("RCM（coreboot）の UEFI は、PC の ROM を自分でビルドし直して更新してください。", "For the RCM (coreboot) UEFI, rebuild the ROM on your PC."));
                return ExitNotHekate;
            }
            if (ReleaseClient.Sha256(cur) == sha)
            {
                log(T($"UEFI はもう {version} です。", $"The UEFI is already {version}."));
                return ExitUpToDate;
            }

            // ---- keep the running UEFI as [Windows (previous UEFI)]
            string prev = Path.Combine(esp, PrevDir);
            Directory.CreateDirectory(prev);
            CopyChecked(cur, Path.Combine(prev, "bl33.bin"));
            string bl31 = Path.Combine(esp, Dir, "bl31.bin");
            if (File.Exists(bl31)) CopyChecked(bl31, Path.Combine(prev, "bl31.bin"));
            log(T("今の UEFI を switchroot\\switchwoa-prev に残しました。", "Kept the current UEFI in switchroot\\switchwoa-prev."));
            AddPreviousEntry(Path.Combine(esp, @"bootloader\hekate_ipl.ini"), log);

            // ---- the new bl33.bin: copy next to it, check, then replace
            string tmp = cur + ".new";
            CopyChecked(bl33, tmp);
            File.SetAttributes(cur, FileAttributes.Normal);
            File.Move(tmp, cur, true);
            if (ReleaseClient.Sha256(cur) != sha)
                throw new IOException(T("ESP の bl33.bin が、置いたあとで合いません。hekate の「Windows (previous UEFI)」で起動してください。",
                                        "bl33.bin on the ESP does not match after the copy. Boot \"Windows (previous UEFI)\" from hekate."));
            log(T($"UEFI を {version} にしました（switchroot\\switchwoa\\bl33.bin）。", $"Updated the UEFI to {version} (switchroot\\switchwoa\\bl33.bin)."));
            log(T("次に hekate から Windows を起動したときに、新しい UEFI になります。起動しなかったら、hekate で「Windows (previous UEFI)」を選んでください。",
                  "The new UEFI starts the next time Windows is booted from hekate. If it does not boot, choose \"Windows (previous UEFI)\" in hekate."));
            return ExitUpdated;
        }

        static void CopyChecked(string src, string dest)
        {
            if (File.Exists(dest)) File.SetAttributes(dest, FileAttributes.Normal);
            File.Copy(src, dest, true);
            if (ReleaseClient.Sha256(dest) != ReleaseClient.Sha256(src))
                throw new IOException(T($"{Path.GetFileName(dest)} のコピーが元と合いません。", $"The copy of {Path.GetFileName(dest)} does not match its source."));
        }

        /// <summary>Adds the previous-UEFI entry to hekate_ipl.ini once (the old file kept as .bak-time).</summary>
        static void AddPreviousEntry(string ini, Action<string> log)
        {
            if (!File.Exists(ini)) { log(T("hekate_ipl.ini がないので、前の UEFI の項目は足していません。", "No hekate_ipl.ini: the previous-UEFI entry was not added.")); return; }
            string text = File.ReadAllText(ini);
            if (Regex.IsMatch(text, @"^\s*boot_prefixes\s*=\s*switchroot/switchwoa-prev/\s*$", RegexOptions.Multiline | RegexOptions.IgnoreCase)) return;
            File.Copy(ini, ini + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"), true);
            File.AppendAllText(ini, (text.EndsWith("\n") ? "\r\n" : "\r\n\r\n") + PrevEntry, new UTF8Encoding(false));
            log(T("hekate_ipl.ini に「Windows (previous UEFI)」を足しました。", "Added \"Windows (previous UEFI)\" to hekate_ipl.ini."));
        }

        /// <summary>Mounts the EFI system partition on a free drive letter (mountvol /s). Returns the letter.</summary>
        static string MountEsp(Action<string> log)
        {
            string used = new string(DriveInfo.GetDrives().Select(d => char.ToUpperInvariant(d.Name[0])).ToArray());
            char free = "STUVWXYZRQPONMLKJIHG".FirstOrDefault(c => used.IndexOf(c) < 0);
            if (free == '\0') throw new IOException(T("空いているドライブ文字がありません。", "No free drive letter."));
            if (Mountvol($"{free}: /s") != 0 || !Directory.Exists(free + @":\"))
                throw new IOException(T("ESP（EFI システム区画）を開けませんでした（mountvol /s）。", "Could not mount the EFI system partition (mountvol /s)."));
            log(T($"ESP を {free}: で開きました。", $"Mounted the ESP on {free}:."));
            return free.ToString();
        }

        static void UnmountEsp(string letter, Action<string> log)
        {
            if (Mountvol($"{letter}: /d") != 0) log(T($"{letter}: を閉じられませんでした（再起動で消えます）。", $"Could not unmount {letter}: (gone after a restart)."));
        }

        static int Mountvol(string args)
        {
            using Process p = Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "mountvol.exe"), args) { UseShellExecute = false, CreateNoWindow = true });
            p.WaitForExit(30000);
            return p.HasExited ? p.ExitCode : -1;
        }
    }

    /// <summary>The elevated process's window for the UEFI update.</summary>
    sealed class UefiUpdateForm : Form
    {
        readonly TextBox logBox = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical };
        readonly Button close = new Button { Dock = DockStyle.Bottom, Height = 48, Enabled = false };
        public int Result { get; private set; } = UefiUpdate.ExitFailed;

        public UefiUpdateForm(string tag)
        {
            Text = T("UEFI の更新（hekate）", "UEFI update (hekate)");
            Font = new System.Drawing.Font("Yu Gothic UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new System.Drawing.Size(560, 480);
            close.Text = T("閉じる", "Close");
            close.Click += (_, _) => Close();
            Controls.Add(logBox);
            Controls.Add(close);
            FormClosing += (_, e) => { if (!close.Enabled && e.CloseReason == CloseReason.UserClosing) e.Cancel = true; };
            Shown += async (_, _) =>
            {
                try
                {
                    Result = await UefiUpdate.RunAsync(tag, Log);
                }
                catch (Exception e)
                {
                    Result = UefiUpdate.ExitFailed;
                    Log("");
                    Log(T("中止しました: ", "Stopped: ") + e.Message);
                }
                close.Enabled = true;
            };
        }

        void Log(string line) => logBox.AppendText(line + Environment.NewLine);
    }
}
