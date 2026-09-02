#pragma once

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    /// <summary>
    /// Dedicated updater-specific markdown renderer.
    /// Independent, small, deterministic, easy to test.
    /// Renders GitHub release notes (UTF-16 String) into a RichTextBox without executing HTML/JS.
    /// Supports: headings, bold, italic, inline code, code blocks, unordered/ordered lists, links, emoji, line breaks, hr, blockquote.
    /// Emoji: preserves surrogate pairs, uses Segoe UI Emoji fallback where needed.
    /// </summary>
    public ref class MarkdownRenderer sealed
    {
    public:
        static void Render(System::Windows::Forms::RichTextBox^ rtb, System::String^ markdown);

    private:
        static void AppendHeading(System::Windows::Forms::RichTextBox^ rtb, System::String^ text, int level);
        static void AppendParagraph(System::Windows::Forms::RichTextBox^ rtb, System::String^ text);
        static void ParseInline(System::Windows::Forms::RichTextBox^ rtb, System::String^ text);
        static void AppendCodeBlock(System::Windows::Forms::RichTextBox^ rtb, System::String^ code);
        static void AppendListItem(System::Windows::Forms::RichTextBox^ rtb, System::String^ text, bool ordered, int number);
        static void AppendBlockquote(System::Windows::Forms::RichTextBox^ rtb, System::String^ text);
        static void AppendHorizontalRule(System::Windows::Forms::RichTextBox^ rtb);
        static void AppendTextWithStyle(System::Windows::Forms::RichTextBox^ rtb, System::String^ text, System::Drawing::FontStyle style, System::Drawing::Color color, bool isCode);
        static bool IsSurrogatePair(System::String^ s, int index);
        static System::Drawing::Font^ GetFontForRun(System::String^ text, System::Drawing::Font^ baseFont, System::Drawing::FontStyle style, bool isCode);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
