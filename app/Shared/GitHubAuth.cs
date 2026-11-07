// GitHub sign-in for reading the project's releases while its repositories are
// private (shared by SwitchCenter and SwitchPatcher).
//
// The "SwitchCenter" GitHub App (Contents: read-only, installed only on the
// project's repositories) is signed in with the OAuth device flow: the app shows
// a code, the user enters it at github.com/login/device. Only the App's client
// ID is in the program; there is no client secret. The user-to-server token can
// read only repositories where the App is installed and that the user may read
// (people are invited as collaborators), and it expires; an expired token is
// renewed with the refresh token when GitHub allows that without a secret,
// otherwise the user signs in again.
//
// The tokens are kept in %LOCALAPPDATA%\SwitchWOA\github.bin, encrypted with
// DPAPI for the current user (the elevated installer runs as the same user and
// can read it; other accounts cannot).
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    /// <summary>The release needs a GitHub sign-in (the repository is private, or the token was refused).</summary>
    sealed class SignInRequiredException : Exception
    {
        public SignInRequiredException() : base(T("このリリースを読むには、GitHub でのサインインが必要です。",
                                                  "Reading this release needs a GitHub sign-in.")) { }
    }

    static class GitHubAuth
    {
        public const string ClientId = "Iv23lipHmNQo23vNuyUV";
        static readonly string StorePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SwitchWOA", "github.bin");
        static readonly SemaphoreSlim Gate = new SemaphoreSlim(1, 1);

        sealed class Tokens
        {
            public string Access { get; set; }
            public string Refresh { get; set; }
            public DateTime AccessExpiresUtc { get; set; }      // DateTime.MaxValue = does not expire
            public DateTime RefreshExpiresUtc { get; set; }
            public string Login { get; set; }
        }

        public sealed class DeviceCode
        {
            public string Device, User, VerificationUri;
            public int IntervalSeconds;
            public DateTime ExpiresUtc;
        }

        /// <summary>The signed-in GitHub account, or null.</summary>
        public static string Login => Load()?.Login;
        public static bool SignedIn => Load() is Tokens t && (t.AccessExpiresUtc > DateTime.UtcNow || (t.Refresh != null && t.RefreshExpiresUtc > DateTime.UtcNow));

        public static void SignOut()
        {
            try { File.Delete(StorePath); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        /// <summary>A usable access token, renewed if needed; null when not signed in (or the sign-in has run out).</summary>
        public static async Task<string> GetTokenAsync()
        {
            await Gate.WaitAsync();
            try
            {
                Tokens t = Load();
                if (t == null) return null;
                if (t.AccessExpiresUtc > DateTime.UtcNow.AddMinutes(5)) return t.Access;
                if (t.Refresh == null || t.RefreshExpiresUtc <= DateTime.UtcNow) return null;
                Dictionary<string, string> r = await PostAsync("https://github.com/login/oauth/access_token", new Dictionary<string, string>
                {
                    ["client_id"] = ClientId,
                    ["grant_type"] = "refresh_token",
                    ["refresh_token"] = t.Refresh,
                });
                if (!r.ContainsKey("access_token")) return null;        // GitHub wants a secret for this, or it was revoked: sign in again
                Tokens renewed = FromResponse(r);
                renewed.Login = t.Login;
                Save(renewed);
                return renewed.Access;
            }
            catch (HttpRequestException) { return null; }
            finally { Gate.Release(); }
        }

        /// <summary>Step 1 of the device flow: the code to show the user.</summary>
        public static async Task<DeviceCode> StartAsync()
        {
            Dictionary<string, string> r = await PostAsync("https://github.com/login/device/code", new Dictionary<string, string> { ["client_id"] = ClientId });
            if (!r.TryGetValue("device_code", out string device) || !r.TryGetValue("user_code", out string user))
                throw new InvalidDataException(T("GitHub からコードを受け取れませんでした: ", "GitHub did not return a code: ") + (r.TryGetValue("error", out string e) ? e : "?"));
            string uri = r.TryGetValue("verification_uri", out string u) && u.StartsWith("https://github.com/", StringComparison.Ordinal) ? u : "https://github.com/login/device";
            return new DeviceCode
            {
                Device = device,
                User = user,
                VerificationUri = uri,
                IntervalSeconds = r.TryGetValue("interval", out string i) && int.TryParse(i, out int s) && s > 0 ? s : 5,
                ExpiresUtc = DateTime.UtcNow.AddSeconds(r.TryGetValue("expires_in", out string x) && int.TryParse(x, out int sec) ? sec : 900),
            };
        }

        /// <summary>Step 2: waits until the user has entered the code. True when signed in, false when declined or expired.</summary>
        public static async Task<bool> WaitAsync(DeviceCode code, CancellationToken cancel)
        {
            int interval = code.IntervalSeconds;
            while (DateTime.UtcNow < code.ExpiresUtc)
            {
                await Task.Delay(TimeSpan.FromSeconds(interval), cancel);
                Dictionary<string, string> r = await PostAsync("https://github.com/login/oauth/access_token", new Dictionary<string, string>
                {
                    ["client_id"] = ClientId,
                    ["device_code"] = code.Device,
                    ["grant_type"] = "urn:ietf:params:oauth:grant-type:device_code",
                });
                if (r.ContainsKey("access_token"))
                {
                    Tokens t = FromResponse(r);
                    t.Login = await ReadLoginAsync(t.Access);
                    Save(t);
                    return true;
                }
                string error = r.TryGetValue("error", out string e) ? e : "";
                if (error == "authorization_pending") continue;
                if (error == "slow_down") { interval += 5; continue; }
                return false;                                           // expired_token, access_denied, ...
            }
            return false;
        }

        static Tokens FromResponse(Dictionary<string, string> r)
        {
            DateTime now = DateTime.UtcNow;
            return new Tokens
            {
                Access = r["access_token"],
                Refresh = r.TryGetValue("refresh_token", out string rt) && rt.Length > 0 ? rt : null,
                AccessExpiresUtc = r.TryGetValue("expires_in", out string e) && int.TryParse(e, out int s) && s > 0 ? now.AddSeconds(s) : DateTime.MaxValue,
                RefreshExpiresUtc = r.TryGetValue("refresh_token_expires_in", out string re) && int.TryParse(re, out int rs) && rs > 0 ? now.AddSeconds(rs) : DateTime.MaxValue,
            };
        }

        static async Task<string> ReadLoginAsync(string token)
        {
            try
            {
                using HttpClient http = ReleaseClient.NewClient(token);
                using JsonDocument d = JsonDocument.Parse(await http.GetStringAsync("https://api.github.com/user"));
                return d.RootElement.GetProperty("login").GetString();
            }
            catch (Exception ex) when (ex is HttpRequestException || ex is JsonException || ex is KeyNotFoundException) { return "?"; }
        }

        /// <summary>Form POST with Accept: application/json; returns the top-level fields as strings.</summary>
        static async Task<Dictionary<string, string>> PostAsync(string url, Dictionary<string, string> form)
        {
            using HttpClient http = ReleaseClient.NewClient(null);
            using var request = new HttpRequestMessage(HttpMethod.Post, url) { Content = new FormUrlEncodedContent(form) };
            request.Headers.Accept.ParseAdd("application/json");
            using HttpResponseMessage response = await http.SendAsync(request);
            string body = await response.Content.ReadAsStringAsync();
            var result = new Dictionary<string, string>();
            try
            {
                using JsonDocument d = JsonDocument.Parse(body);
                foreach (JsonProperty p in d.RootElement.EnumerateObject())
                    result[p.Name] = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString() : p.Value.ToString();
            }
            catch (JsonException) { result["error"] = "bad_response_" + (int)response.StatusCode; }
            return result;
        }

        // ------------------------------------------------------------ storage (DPAPI, current user)

        static Tokens Load()
        {
            try
            {
                if (!File.Exists(StorePath)) return null;
                byte[] plain = Unprotect(File.ReadAllBytes(StorePath));
                return plain == null ? null : JsonSerializer.Deserialize<Tokens>(plain);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is JsonException) { return null; }
        }

        static void Save(Tokens t)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(StorePath));
            File.WriteAllBytes(StorePath, Protect(JsonSerializer.SerializeToUtf8Bytes(t)));
        }

        [StructLayout(LayoutKind.Sequential)]
        struct DATA_BLOB { public int cbData; public IntPtr pbData; }
        [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool CryptProtectData(ref DATA_BLOB pDataIn, string szDataDescr, IntPtr pOptionalEntropy, IntPtr pvReserved, IntPtr pPromptStruct, int dwFlags, out DATA_BLOB pDataOut);
        [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool CryptUnprotectData(ref DATA_BLOB pDataIn, IntPtr ppszDataDescr, IntPtr pOptionalEntropy, IntPtr pvReserved, IntPtr pPromptStruct, int dwFlags, out DATA_BLOB pDataOut);
        [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr hMem);
        const int CRYPTPROTECT_UI_FORBIDDEN = 0x1;

        static byte[] Protect(byte[] data) => Crypt(data, true) ?? throw new Win32Exception(Marshal.GetLastWin32Error());
        static byte[] Unprotect(byte[] data) => Crypt(data, false);

        static byte[] Crypt(byte[] data, bool protect)
        {
            GCHandle pin = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                var input = new DATA_BLOB { cbData = data.Length, pbData = pin.AddrOfPinnedObject() };
                bool ok = protect
                    ? CryptProtectData(ref input, "SwitchWOA GitHub", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, CRYPTPROTECT_UI_FORBIDDEN, out DATA_BLOB output)
                    : CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, CRYPTPROTECT_UI_FORBIDDEN, out output);
                if (!ok) return null;
                try
                {
                    var result = new byte[output.cbData];
                    Marshal.Copy(output.pbData, result, 0, output.cbData);
                    return result;
                }
                finally { LocalFree(output.pbData); }
            }
            finally { pin.Free(); }
        }
    }

    /// <summary>Shows the device code, opens github.com/login/device and waits for the sign-in.</summary>
    sealed class GitHubSignInForm : Form
    {
        readonly Label info = new Label { Dock = DockStyle.Top, Height = 110, Padding = new Padding(12) };
        readonly TextBox code = new TextBox { Dock = DockStyle.Top, ReadOnly = true, TextAlign = HorizontalAlignment.Center, Font = new System.Drawing.Font("Consolas", 22f, System.Drawing.FontStyle.Bold) };
        readonly Button open = new Button { Dock = DockStyle.Top, Height = 48 };
        readonly Label status = new Label { Dock = DockStyle.Fill, Padding = new Padding(12) };
        readonly CancellationTokenSource cancel = new CancellationTokenSource();
        GitHubAuth.DeviceCode device;

        public GitHubSignInForm()
        {
            Text = T("GitHub でサインイン", "Sign in with GitHub");
            Font = new System.Drawing.Font("Yu Gothic UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new System.Drawing.Size(520, 330);
            MinimizeBox = MaximizeBox = false;
            info.Text = T("このプロジェクトのリリースは、招待された GitHub アカウントだけが読めます。\n" +
                          "「ブラウザで開く」を押し、下のコードを入れて「SwitchCenter」を許可してください。（コードはコピー済みです）",
                          "Only invited GitHub accounts can read this project's releases.\n" +
                          "Press \"Open in browser\", enter the code below and authorize \"SwitchCenter\". (The code is already copied.)");
            open.Text = T("ブラウザで開く", "Open in browser");
            open.Enabled = false;
            open.Click += (_, _) => { if (device != null) OpenBrowser(device.VerificationUri); };
            status.Text = T("GitHub からコードを受け取っています…", "Getting a code from GitHub...");
            Controls.Add(status);
            Controls.Add(open);
            Controls.Add(code);
            Controls.Add(info);
            FormClosed += (_, _) => cancel.Cancel();
            Shown += async (_, _) => await Run();
        }

        async Task Run()
        {
            try
            {
                device = await GitHubAuth.StartAsync();
                code.Text = device.User;
                try { Clipboard.SetText(device.User); } catch (ExternalException) { }
                open.Enabled = true;
                status.Text = T($"{device.VerificationUri} でコードを入れるのを待っています…", $"Waiting for the code to be entered at {device.VerificationUri}...");
                OpenBrowser(device.VerificationUri);
                if (await GitHubAuth.WaitAsync(device, cancel.Token))
                {
                    DialogResult = DialogResult.OK;
                    Close();
                    return;
                }
                status.ForeColor = System.Drawing.Color.Firebrick;
                status.Text = T("サインインできませんでした（拒否されたか、時間切れです）。閉じて、もう一度やり直してください。",
                                "Sign-in did not complete (declined or timed out). Close and try again.");
            }
            catch (OperationCanceledException) { }
            catch (Exception e) when (e is HttpRequestException || e is InvalidDataException || e is TaskCanceledException)
            {
                status.ForeColor = System.Drawing.Color.Firebrick;
                status.Text = T("GitHub につながりませんでした: ", "Could not reach GitHub: ") + e.Message;
            }
        }

        /// <summary>Edge if it is installed (the Switch's default browser can be IE11, which GitHub does not support).</summary>
        static void OpenBrowser(string url)
        {
            foreach (Environment.SpecialFolder f in new[] { Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.ProgramFiles })
            {
                string edge = Path.Combine(Environment.GetFolderPath(f), @"Microsoft\Edge\Application\msedge.exe");
                if (File.Exists(edge)) { Process.Start(edge, url)?.Dispose(); return; }
            }
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose();
        }
    }
}
