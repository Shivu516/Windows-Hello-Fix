#pragma once

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    /// <summary>
    /// Dedicated updater-specific HTML rendering pipeline.
    /// Architecture: GitHub Release Body (markdown) → MarkdownToHtml (controlled) → SanitizeHtml → HtmlToRtf (RichTextBox).
    /// Only allow-list tags are generated; release body is treated as data, not executable HTML.
    /// Supports: h1/h2/h3, p, strong, em, code, pre, ul/ol/li, a (https only), blockquote, hr, br, emoji/Unicode.
    /// Replaces MarkdownRenderer — prefer only HTML for rendering (per prompt).
    /// </summary>
    public ref class HtmlRenderer sealed
    {
    public:
        // Main entry: markdown (GitHub release body) → safe HTML → native RichTextBox preview
        static void Render(System::Windows::Forms::RichTextBox^ rtb, System::String^ markdown);

        // Exposed for testing: markdown → controlled HTML (allow-list tags only)
        static System::String^ MarkdownToHtml(System::String^ markdown);

        // Sanitize HTML: strip dangerous tags/attributes, allow only safe subset
        static System::String^ SanitizeHtml(System::String^ html);

    private:
        static void HtmlToRtf(System::Windows::Forms::RichTextBox^ rtb, System::String^ html);
        static System::String^ EscapeHtml(System::String^ text);
        static System::String^ UnescapeHtml(System::String^ text);
        static bool IsSafeUrl(System::String^ url);

        // Helpers for RTF emission (reused from MarkdownRenderer, now driven by HTML tags)
        static void AppendHeading(System::Windows::Forms::RichTextBox^ rtb, System::String^ text, int level);
        static void AppendCodeBlock(System::Windows::Forms::RichTextBox^ rtb, System::String^ code);
        static void AppendHorizontalRule(System::Windows::Forms::RichTextBox^ rtb);
        static void AppendTextWithStyle(System::Windows::Forms::RichTextBox^ rtb, System::String^ text, System::Drawing::FontStyle style, System::Drawing::Color color, bool isCode);
        static System::Drawing::Font^ GetFontForRun(System::String^ text, System::Drawing::Font^ baseFont, System::Drawing::FontStyle style, bool isCode);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
