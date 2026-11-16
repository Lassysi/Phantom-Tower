using System.Globalization;
using System.Reflection;

namespace PBZ.Tower.UI;

public sealed class LoaderForm : Form
{
    public static readonly Dictionary<string, (string Zh, string En)> Copy = new()
    {
        ["title"] = ("影塔·百战", "SHADOW TOWER"), ["subtitle"] = ("模组加载器", "MOD LOADER"),
        ["load"] = ("加载影塔", "Load mod"), ["reconnect"] = ("重新连接", "Reconnect"), ["close"] = ("关闭", "Close"),
        ["idle"] = ("准备就绪", "Ready"), ["idle.body"] = ("启动游戏后加载", "Start the game, then load the mod."),
        ["wait_game"] = ("等待游戏", "Waiting for game"), ["wait_game.body"] = ("请启动《影之刃零》", "Start Phantom Blade Zero."),
        ["loading_core"] = ("加载运行库…", "Loading runtime…"), ["wait_core"] = ("等待运行库", "Waiting for runtime"),
        ["wait_core.body"] = ("正在初始化", "Initializing…"), ["loading_module"] = ("加载影塔…", "Loading mod…"),
        ["connected"] = ("已加载", "Loaded"), ["connected.body"] = ("可关闭此窗口", "You can close this window."),
        ["working.body"] = ("请稍候", "Please wait."), ["error.body"] = ("查看详情后重试", "Check details and retry."),
        ["loader_version"] = ("加载器", "Loader"), ["engine_version"] = ("引擎", "Engine"),
        ["development"] = ("开发版", "Development"), ["preview"] = ("预览", "Preview"),
        ["details"] = ("详情", "Details"), ["about"] = ("说明", "Guide"),
        ["abi_mismatch"] = ("版本不兼容", "Incompatible runtime"), ["game_version"] = ("游戏版本未适配", "Unsupported game version"),
        ["multiple_games"] = ("请只运行一个游戏", "Keep one game instance"), ["access_denied"] = ("无法访问游戏进程", "Cannot access game process"),
        ["wrong_pipe"] = ("连接的进程不匹配", "Wrong game process"), ["module_failed"] = ("模块加载失败", "Module load failed"),
        ["package_missing"] = ("缺少引擎文件", "Engine files missing"), ["package_invalid"] = ("文件校验失败", "File verification failed"),
        ["inject_failed"] = ("运行库加载失败", "Runtime load failed"), ["inject_timeout"] = ("运行库加载超时", "Runtime load timed out"),
        ["inject_once"] = ("请重启游戏后重试", "Restart game and retry"), ["core_changed"] = ("运行库已更新", "Runtime changed"),
        ["disconnected"] = ("连接已断开", "Disconnected"), ["response_timeout"] = ("响应超时", "Response timed out"),
        ["connection_failed"] = ("连接失败", "Connection failed"), ["bad_message"] = ("通信异常", "Invalid response"),
        ["message_too_large"] = ("通信异常", "Invalid response"), ["truncated_message"] = ("通信中断", "Connection interrupted"),
        ["unsafe_path"] = ("组件路径无效", "Invalid component path"), ["extract_busy"] = ("组件正在准备中", "Preparing components"),
        ["game_changed"] = ("游戏进程已变化", "Game process changed"), ["save_not_found"] = ("未找到游戏目录", "Game folder not found")
    };
    readonly PackageFiles files;
    readonly bool mock;
    readonly Label title = MakeLabel("Title"), subtitle = MakeLabel("Subtitle"), preview = MakeLabel("Preview"), status = MakeLabel("Status"), hint = MakeLabel("Hint"), versions = MakeLabel("Versions");
    readonly Button load = new() { Name = "Load" }, close = new() { Name = "Close" };
    readonly LinkLabel details = new() { Name = "Details", AutoSize = true };
    readonly ComboBox language = new() { Name = "Language", AccessibleName = "语言 / Language" };
    readonly System.Windows.Forms.Timer previewTimer = new() { Interval = 650 };
    readonly PictureBox logo;
    CoreLink? link;
    string lang, code = "idle", detail = "";
    int step;
    bool closing, disposed;
    protected override bool ShowWithoutActivation => mock;
    static readonly Color Bg = Color.FromArgb(21, 24, 29), Surface = Color.FromArgb(32, 37, 45), Gold = Color.FromArgb(255, 208, 68), Muted = Color.FromArgb(168, 175, 186);
    static Label MakeLabel(string name) => new() { Name = name, AutoSize = true, Dock = DockStyle.Fill, Margin = Padding.Empty, UseMnemonic = false };
    public LoaderForm(PackageFiles files, bool mock, string scenario = "connected", int quitAfter = 0)
    {
        this.files = files; this.mock = mock;
        lang = CultureInfo.CurrentUICulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase) ? "zh-CN" : "en";
        var languagePath = Path.Combine(files.UserRoot, "loader-language.txt");
        try { PackageFiles.RejectLinks(languagePath); var saved = File.ReadAllText(languagePath).Trim(); if (saved is "zh-CN" or "en") lang = saved; } catch (IOException) { }
        AutoScaleDimensions = new SizeF(96, 96); AutoScaleMode = AutoScaleMode.Dpi;
        Font = new("Segoe UI", 9); ForeColor = Color.FromArgb(244, 241, 231); BackColor = Bg;
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowOnly; ClientSize = new(560, 310);
        FormBorderStyle = FormBorderStyle.Sizable; MaximizeBox = true; StartPosition = FormStartPosition.CenterScreen;
        Shown += (_, _) => MinimumSize = Size;
        using var iconStream = Assembly.GetExecutingAssembly().GetManifestResourceStream("PBZTower.Fox.ico")!;
        Icon = new Icon(iconStream);
        using var imageStream = Assembly.GetExecutingAssembly().GetManifestResourceStream("PBZTower.Fox.png")!;
        using var image = Image.FromStream(imageStream);
        logo = new PictureBox { Name = "FoxLogo", Image = new Bitmap(image), SizeMode = PictureBoxSizeMode.Zoom, Size = new(56, 56), Margin = new Padding(0, 0, 16, 0) };
        var root = new TableLayoutPanel { Name = "Root", Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new(560, 0), Padding = new Padding(24), ColumnCount = 1, RowCount = 5 };
        for (int i = 0; i < 5; i++) root.RowStyles.Add(new(SizeType.AutoSize));
        var header = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        header.ColumnStyles.Add(new(SizeType.AutoSize)); header.ColumnStyles.Add(new(SizeType.Percent, 100)); header.ColumnStyles.Add(new(SizeType.Absolute, 90));
        var brand = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, RowCount = 2, Margin = new Padding(0, 0, 14, 0) };
        title.Font = new("Microsoft YaHei UI", 18, FontStyle.Bold); title.ForeColor = Gold; title.Anchor = AnchorStyles.Left;
        subtitle.ForeColor = Muted; subtitle.Font = new("Segoe UI", 8); subtitle.Margin = new Padding(1, 5, 0, 0);
        brand.Controls.Add(title, 0, 0); brand.Controls.Add(subtitle, 0, 1);
        language.DropDownStyle = ComboBoxStyle.DropDownList; language.Dock = DockStyle.Top; language.Margin = new Padding(0, 3, 0, 0);
        language.Items.AddRange(["中文", "English"]); language.SelectedIndex = lang == "zh-CN" ? 0 : 1;
        language.SelectedIndexChanged += (_, _) => { lang = language.SelectedIndex == 0 ? "zh-CN" : "en"; PackageFiles.RejectLinks(languagePath); File.WriteAllText(languagePath, lang); RefreshCopy(); };
        header.Controls.Add(logo, 0, 0); header.Controls.Add(brand, 1, 0); header.Controls.Add(language, 2, 0); root.Controls.Add(header, 0, 0);
        var stateBox = new TableLayoutPanel { Name = "StateBox", Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, RowCount = 2, Padding = new Padding(16), BackColor = Surface, Margin = new Padding(0, 22, 0, 0) };
        status.Font = new("Segoe UI", 14, FontStyle.Bold); hint.ForeColor = Muted; hint.Margin = new Padding(0, 8, 0, 0);
        stateBox.Controls.Add(status, 0, 0); stateBox.Controls.Add(hint, 0, 1); root.Controls.Add(stateBox, 0, 1);
        versions.ForeColor = Muted; versions.Font = new("Segoe UI", 8); versions.Margin = new Padding(0, 16, 0, 16); root.Controls.Add(versions, 0, 2);
        var buttons = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        buttons.ColumnStyles.Add(new(SizeType.Percent, 65)); buttons.ColumnStyles.Add(new(SizeType.Percent, 35));
        foreach (var b in new[] { load, close }) { b.Dock = DockStyle.Fill; b.Height = 42; b.MinimumSize = new(0, 42); b.FlatStyle = FlatStyle.Flat; b.Margin = Padding.Empty; }
        load.BackColor = Gold; load.ForeColor = Bg; load.FlatAppearance.BorderSize = 0; load.Font = new("Segoe UI", 10, FontStyle.Bold); load.Click += (_, _) => LoadRuntime(scenario);
        close.BackColor = Surface; close.FlatAppearance.BorderColor = Color.FromArgb(56, 66, 78); close.Margin = new Padding(12, 0, 0, 0); close.Click += (_, _) => Close();
        buttons.Controls.Add(load, 0, 0); buttons.Controls.Add(close, 1, 0); root.Controls.Add(buttons, 0, 3);
        var bottom = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = new Padding(0, 12, 0, 0) };
        bottom.ColumnStyles.Add(new(SizeType.Percent, 100)); bottom.ColumnStyles.Add(new(SizeType.AutoSize));
        preview.ForeColor = Muted; preview.Font = new("Segoe UI", 8); details.LinkColor = Muted; details.ActiveLinkColor = Gold; details.Anchor = AnchorStyles.Right;
        details.LinkClicked += (_, _) => ShowDetails(); bottom.Controls.Add(preview, 0, 0); bottom.Controls.Add(details, 1, 0); root.Controls.Add(bottom, 0, 4); Controls.Add(root);
        previewTimer.Tick += (_, _) => { step++; code = step < 2 ? "wait_game" : step < 3 ? "loading_core" : step < 4 ? "loading_module" : scenario == "abi" ? "abi_mismatch" : "connected"; if (step >= 4) previewTimer.Stop(); RefreshCopy(); };
        FormClosing += ClosingAsync; RefreshCopy();
        if (mock && quitAfter > 0) { var deadline = new System.Windows.Forms.Timer { Interval = Math.Clamp(quitAfter, 500, 180000) }; deadline.Tick += (_, _) => { deadline.Stop(); deadline.Dispose(); Close(); }; deadline.Start(); }
    }
    string T(string key) => Copy.TryGetValue(key.StartsWith("ui.") ? key[3..] : key, out var text) ? lang == "zh-CN" ? text.Zh : text.En : lang == "zh-CN" ? "加载失败" : "Load failed";
    bool IsError => detail.Length > 0 || !(code is "idle" or "wait_game" or "loading_core" or "wait_core" or "loading_module" or "connected");
    void RefreshCopy()
    {
        SuspendLayout(); Text = lang == "zh-CN" ? "影塔·百战" : "Shadow Tower"; title.Text = T("title"); subtitle.Text = T("subtitle");
        status.Text = T(code); hint.Text = Copy.ContainsKey(code + ".body") ? T(code + ".body") : T(code is "loading_core" or "loading_module" ? "working.body" : "error.body");
        status.ForeColor = code == "connected" ? Color.FromArgb(70, 215, 242) : IsError ? Color.FromArgb(255, 129, 122) : ForeColor;
        versions.Text = $"{T("loader_version")} 0.2.0     ·     {T("engine_version")} {files.Release?.EngineVersion ?? T("development")}";
        load.Text = T(link is null && !previewTimer.Enabled && step == 0 ? "load" : "reconnect"); close.Text = T("close"); preview.Text = mock ? T("preview") : ""; details.Text = T(IsError ? "details" : "about"); ResumeLayout(true);
    }
    void ShowDetails()
    {
        if (IsError) { MessageBox.Show(this, T(code) + (detail.Length > 0 ? "\n\n" + detail : ""), T("details"), MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
        var path = Path.Combine(files.Assets, "README.txt");
        using var guide = new Form { Text = T("about"), Size = new(760, 620), StartPosition = FormStartPosition.CenterParent, Icon = Icon };
        guide.Controls.Add(new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Text = File.Exists(path) ? File.ReadAllText(path) : $"Shadow Tower 0.2.0\r\nABI {Wire.Abi}", Font = new("Segoe UI", 9) }); guide.ShowDialog(this);
    }
    void LoadRuntime(string scenario)
    {
        if (mock) { step = 0; previewTimer.Start(); return; }
        if (link is null) { link = new(files); link.Status += OnStatus; link.Start(); } else link.Reconnect(); code = "wait_game"; detail = ""; RefreshCopy();
    }
    void OnStatus(LinkStatus value) { if (!disposed && IsHandleCreated) BeginInvoke(() => { if (!disposed) { code = value.Code; detail = value.Detail; RefreshCopy(); } }); }
    async void ClosingAsync(object? sender, FormClosingEventArgs e)
    {
        if (disposed) return; e.Cancel = true; if (closing) return; closing = true; load.Enabled = close.Enabled = false; previewTimer.Stop(); previewTimer.Dispose();
        if (link is not null) { link.Status -= OnStatus; await link.DisposeAsync(); } logo.Image?.Dispose(); disposed = true; Close();
    }
}
