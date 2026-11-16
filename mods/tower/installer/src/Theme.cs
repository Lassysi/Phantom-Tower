// 配色与自绘控件：墨黑底、狐狸金主色、朱红笔触、机械眼青色点缀
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace PBZTowerSetup
{
    static class Theme
    {
        public static readonly Color Ink = Color.FromArgb(18, 17, 16);
        public static readonly Color Panel = Color.FromArgb(28, 26, 24);
        public static readonly Color Line = Color.FromArgb(58, 52, 46);
        public static readonly Color Paper = Color.FromArgb(232, 224, 210);
        public static readonly Color Muted = Color.FromArgb(150, 140, 126);
        public static readonly Color Gold = Color.FromArgb(240, 180, 46);
        public static readonly Color Crimson = Color.FromArgb(160, 32, 36);
        public static readonly Color Cyan = Color.FromArgb(70, 206, 236);

        static readonly string Brush = Has("KaiTi") ? "KaiTi" : Has("STKaiti") ? "STKaiti" : "Microsoft YaHei UI";
        static readonly string Body = Has("Microsoft YaHei UI") ? "Microsoft YaHei UI" : "Segoe UI";

        public static Font Title(float size) { return new Font(Brush, size, FontStyle.Bold); }
        public static Font Text(float size, FontStyle style = FontStyle.Regular) { return new Font(Body, size, style); }

        static bool Has(string family)
        {
            using (var fonts = new InstalledFontCollection())
                foreach (var f in fonts.Families) if (string.Equals(f.Name, family, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            var p = new GraphicsPath();
            var d = radius * 2;
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }
    }

    // 自绘按钮：Filled = 金底黑字（主操作），否则描边
    sealed class InkButton : Control
    {
        public bool Filled;
        public Color Accent = Theme.Gold;
        bool _hover, _down;

        public InkButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor, true);
            Cursor = Cursors.Hand;
            BackColor = Color.Transparent;
            Font = Theme.Text(11f, FontStyle.Bold);
        }

        protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hover = _down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { _down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { _down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            var accent = Enabled ? Accent : Theme.Line;
            using (var path = Theme.Round(r, 6))
            {
                if (Filled)
                {
                    var fill = !Enabled ? Theme.Line : _down ? ControlPaint.Dark(accent, 0.1f) : _hover ? ControlPaint.Light(accent, 0.15f) : accent;
                    using (var b = new SolidBrush(fill)) g.FillPath(b, path);
                }
                else
                {
                    using (var b = new SolidBrush(_hover && Enabled ? Color.FromArgb(36, accent) : Color.FromArgb(10, 255, 255, 255))) g.FillPath(b, path);
                    using (var p = new Pen(accent, 1.4f)) g.DrawPath(p, path);
                }
            }
            var color = !Enabled ? Theme.Muted : Filled ? Theme.Ink : accent;
            TextRenderer.DrawText(g, Text, Font, Rectangle.Round(r), color, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }
    }
}
