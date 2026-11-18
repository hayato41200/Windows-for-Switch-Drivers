// Booting Windows from hekate (L4T chain: hekate -> bl31 -> bl33 = the Switch UEFI).
// On the SD's ESP (the FAT partition hekate reads):
//   bootloader\...                      hekate itself, from its latest official release (optional)
//   bootloader\hekate_ipl.ini           one entry added: [Windows (SwitchWOA)] l4t=1, boot_prefixes, id=SWOAHKT
//   switchroot\switchwoa\bl33.bin       the Switch UEFI built for hekate (this project's release, or a local file)
//   switchroot\switchwoa\bl31.bin       the ARM Trusted Firmware: never shipped here, the user picks their own file
// Old files are kept as <name>.bak-<time> next to them. Nothing else on the ESP is changed.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    sealed class HekateFiles
    {
        public string Bl33;               // checked bl33.bin to copy
        public string Bl33Label;          // "R419H" or the local file name, for the log
        public string Bl31;               // the user's bl31.bin; null = keep the one on the ESP
        public string HekateDir;          // unpacked hekate release (has bootloader\); null = leave hekate alone
        public string HekateVersion;
    }

    static class Hekate
    {
        public const string EntryId = "SWOAHKT";           // switchpwrbtn's restart-to-Windows uses this id
        public const string Prefix = "switchroot/switchwoa/";
        const string HekateRepo = "CTCaer/hekate";
        static readonly Regex HekateZip = new Regex(@"^hekate_ctcaer_[0-9.]+_Nyx_[0-9.]+\.zip$");
        static readonly string[] L4tFiles = { "sc7entry.bin", "sc7exit.bin", "bpmpfw.bin" };

        public static string Entry =>
            "[Windows (SwitchWOA)]\r\nl4t=1\r\nboot_prefixes=" + Prefix + "\r\nid=" + EntryId + "\r\n";

        /// <summary>
        /// A bl33.bin must at least look like this project's hekate build: the stub, then the
        /// UEFI firmware volume (_FVH) at 0x1000 + 0x28. Catches a wrong file picked by mistake.
        /// </summary>
        public static void CheckBl33(string path)
        {
            var info = new FileInfo(path);
            if (info.Length < 0x40000 || info.Length > 0x1000000)
                throw new InvalidDataException(T($"{info.Name} は bl33.bin の大きさではありません（{info.Length} バイト）。", $"{info.Name} is not the size of a bl33.bin ({info.Length} bytes)."));
            byte[] head = new byte[0x1030];
            using (FileStream f = File.OpenRead(path)) f.ReadExactly(head);
            if (Encoding.ASCII.GetString(head, 0x1028, 4) != "_FVH")
                throw new InvalidDataException(T($"{info.Name} は、hekate 用の Switch UEFI（bl33.bin）ではないようです。", $"{info.Name} does not look like the Switch UEFI built for hekate (bl33.bin)."));
        }

        /// <summary>bl31 is a small AArch64 image; refuse files that clearly are not one (an empty file, a zip, a bl33).</summary>
        public static void CheckBl31(string path)
        {
            var info = new FileInfo(path);
            if (info.Length < 0x1000 || info.Length > 0x100000)
                throw new InvalidDataException(T($"{info.Name} は bl31.bin の大きさではありません（{info.Length} バイト）。", $"{info.Name} is not the size of a bl31.bin ({info.Length} bytes)."));
        }

        /// <summary>Downloads the latest official hekate release (GitHub, CTCaer/hekate) and unpacks it under work.</summary>
        public static async Task<(string Dir, string Version)> DownloadLatestAsync(string work, Action<string> log)
        {
            // Public repository: no sign-in token is sent.
            using HttpClient http = SwitchCenter.ReleaseClient.NewClient(null);
            string json = await http.GetStringAsync($"https://api.github.com/repos/{HekateRepo}/releases/latest");
            using JsonDocument rel = JsonDocument.Parse(json);
            string tag = rel.RootElement.GetProperty("tag_name").GetString();
            JsonElement asset = rel.RootElement.GetProperty("assets").EnumerateArray()
                .FirstOrDefault(a => HekateZip.IsMatch(a.GetProperty("name").GetString() ?? ""));
            if (asset.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(T($"hekate {tag} のリリースに、hekate の zip がありません。", $"The hekate {tag} release has no hekate zip."));
            string name = asset.GetProperty("name").GetString();
            string url = asset.GetProperty("browser_download_url").GetString();
            if (!url.StartsWith($"https://github.com/{HekateRepo}/releases/download/", StringComparison.Ordinal))
                throw new InvalidDataException("unexpected hekate download URL " + url);
            log(T($"hekate {tag}（{name}）をダウンロードしています…", $"Downloading hekate {tag} ({name})..."));
            string zip = Path.Combine(work, name);
            using (HttpResponseMessage r = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead))
            {
                r.EnsureSuccessStatusCode();
                using Stream src = await r.Content.ReadAsStreamAsync();
                using FileStream dst = File.Create(zip);
                await src.CopyToAsync(dst);
            }
            string dir = Path.Combine(work, "hekate");
            ZipFile.ExtractToDirectory(zip, dir);
            if (!Directory.Exists(Path.Combine(dir, "bootloader")) || !L4tFiles.All(f => File.Exists(Path.Combine(dir, @"bootloader\sys\l4t", f))))
                throw new InvalidDataException(T($"{name} の中身が、思っていた形（bootloader\\sys\\l4t）ではありません。", $"{name} does not have the expected layout (bootloader\\sys\\l4t)."));
            if (Directory.GetFiles(dir, "hekate_ctcaer_*.bin").Length != 1)
                throw new InvalidDataException(T($"{name} に hekate のペイロード（.bin）がありません。", $"{name} has no hekate payload (.bin)."));
            return (dir, tag);
        }

        /// <summary>Puts hekate (optional), bl33, bl31 and the menu entry on the ESP. Checks before it writes.</summary>
        public static void PutOnEsp(string esp, HekateFiles h, Action<string> log)
        {
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            string dir = Path.Combine(esp, @"switchroot\switchwoa");
            string bl31 = Path.Combine(dir, "bl31.bin");
            if (h.Bl31 == null && !File.Exists(bl31))
                throw new InvalidOperationException(T("ESP に switchroot\\switchwoa\\bl31.bin がありません。bl31.bin を選んでください。",
                                                      "There is no switchroot\\switchwoa\\bl31.bin on the ESP. Choose a bl31.bin."));

            // ---- hekate itself: bootloader\ (sys, res, payloads...) and the payload next to it.
            if (h.HekateDir != null)
            {
                string src = Path.Combine(h.HekateDir, "bootloader");
                int count = 0;
                foreach (string f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                {
                    string to = Path.Combine(esp, "bootloader", Path.GetRelativePath(src, f));
                    Directory.CreateDirectory(Path.GetDirectoryName(to));
                    if (File.Exists(to)) File.SetAttributes(to, FileAttributes.Normal);
                    File.Copy(f, to, true);
                    count++;
                }
                // The payload: at the ESP root to inject from a PC or phone, and as bootloader\update.bin,
                // which hekate's reboot-to-payload (and switchpwrbtn's restart) starts.
                string payload = Directory.GetFiles(h.HekateDir, "hekate_ctcaer_*.bin").Single();
                File.Copy(payload, Path.Combine(esp, Path.GetFileName(payload)), true);
                string update = Path.Combine(esp, @"bootloader\update.bin");
                if (File.Exists(update)) File.SetAttributes(update, FileAttributes.Normal);
                File.Copy(payload, update, true);
                log(T($"hekate {h.HekateVersion} を ESP に置きました（bootloader の {count} ファイル、{Path.GetFileName(payload)}、bootloader\\update.bin）。",
                      $"Put hekate {h.HekateVersion} on the ESP ({count} files in bootloader, {Path.GetFileName(payload)}, bootloader\\update.bin)."));
            }
            foreach (string f in L4tFiles)
                if (!File.Exists(Path.Combine(esp, @"bootloader\sys\l4t", f)))
                    throw new InvalidOperationException(T($"ESP に bootloader\\sys\\l4t\\{f} がありません。hekate を入れる（または新しくする）を選んでください。",
                                                          $"bootloader\\sys\\l4t\\{f} is missing on the ESP. Choose to add (or update) hekate."));

            // ---- bl33 and bl31
            Directory.CreateDirectory(dir);
            PutChecked(h.Bl33, Path.Combine(dir, "bl33.bin"), stamp, log, T($"UEFI（bl33.bin、{h.Bl33Label}）", $"UEFI (bl33.bin, {h.Bl33Label})"));
            if (h.Bl31 != null)
                PutChecked(h.Bl31, bl31, stamp, log, T("bl31.bin（選んだファイル）", "bl31.bin (the file you chose)"));
            else
                log(T("bl31.bin は ESP にあるものをそのまま使います。", "bl31.bin: the one already on the ESP is kept."));

            // ---- the menu entry
            string ini = Path.Combine(esp, @"bootloader\hekate_ipl.ini");
            if (!File.Exists(ini))
            {
                File.WriteAllText(ini, "[config]\r\nautoboot=0\r\nautoboot_list=0\r\nbootwait=3\r\n\r\n" + Entry, new UTF8Encoding(false));
                log(T("bootloader\\hekate_ipl.ini を作り、Windows の項目を入れました。", "Created bootloader\\hekate_ipl.ini with the Windows entry."));
            }
            else
            {
                string text = File.ReadAllText(ini);
                if (Regex.IsMatch(text, @"^\s*boot_prefixes\s*=\s*" + Regex.Escape(Prefix) + @"\s*$", RegexOptions.Multiline | RegexOptions.IgnoreCase))
                {
                    log(T("hekate_ipl.ini には、もう Windows（switchroot/switchwoa）の項目があります。そのままにします。",
                          "hekate_ipl.ini already has the Windows (switchroot/switchwoa) entry; unchanged."));
                    if (!Regex.IsMatch(text, @"^\s*id\s*=\s*" + EntryId + @"\s*$", RegexOptions.Multiline))
                        log(T($"  注意: その項目に id={EntryId} がないと、Windows からの再起動で Windows に直接戻れません。",
                              $"  Note: without id={EntryId} in that entry, a restart from Windows cannot come straight back to Windows."));
                }
                else
                {
                    File.Copy(ini, ini + ".bak-" + stamp);
                    string sep = text.EndsWith("\n") ? "\r\n" : "\r\n\r\n";
                    File.AppendAllText(ini, sep + Entry, new UTF8Encoding(false));
                    log(T($"hekate_ipl.ini に Windows の項目を足しました（元のファイルは hekate_ipl.ini.bak-{stamp}）。",
                          $"Added the Windows entry to hekate_ipl.ini (the old file is hekate_ipl.ini.bak-{stamp})."));
                }
            }
        }

        static void PutChecked(string src, string dest, string stamp, Action<string> log, string what)
        {
            string want = SwitchCenter.ReleaseClient.Sha256(src);
            if (File.Exists(dest))
            {
                if (SwitchCenter.ReleaseClient.Sha256(dest) == want)
                {
                    log(T($"{what}: ESP のものと同じなので、そのままにします。", $"{what}: same as on the ESP; unchanged."));
                    return;
                }
                File.SetAttributes(dest, FileAttributes.Normal);
                File.Copy(dest, dest + ".bak-" + stamp);
            }
            File.Copy(src, dest, true);
            if (SwitchCenter.ReleaseClient.Sha256(dest) != want)
                throw new IOException(T($"ESP に置いた {Path.GetFileName(dest)} が元のファイルと合いません。", $"{Path.GetFileName(dest)} on the ESP does not match its source."));
            log(T($"{what}を switchroot\\switchwoa に置きました（SHA-256 {want.Substring(0, 16)}…）。", $"Put {what} in switchroot\\switchwoa (SHA-256 {want.Substring(0, 16)}...)."));
        }
    }
}
