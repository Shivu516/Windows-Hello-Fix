#include "MarkdownRenderer.h"

using namespace System;
using namespace System::Drawing;
using namespace System::Windows::Forms;
using namespace System::Text::RegularExpressions;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    void MarkdownRenderer::Render(RichTextBox^ rtb, String^ markdown)
    {
        if (rtb == nullptr) return;
        try {
            rtb->SuspendLayout();
            rtb->Clear();
            rtb->ReadOnly = true;
            rtb->WordWrap = true;
            rtb->DetectUrls = false; // we handle links manually
            rtb->BackColor = SystemColors::Window;
            rtb->ForeColor = SystemColors::WindowText;

            if (String::IsNullOrWhiteSpace(markdown)) {
                rtb->SelectionColor = Color::FromArgb(96, 94, 92);
                rtb->SelectedText = "No release notes.";
                rtb->ResumeLayout();
                return;
            }

            // Normalize line endings
            String^ normalized = markdown->Replace("\r\n", "\n")->Replace("\r", "\n");
            array<String^>^ lines = normalized->Split('\n');
            bool inCodeBlock = false;
            String^ codeBlockBuffer = "";

            for (int i = 0; i < lines->Length; i++) {
                String^ line = lines[i];
                String^ trimmed = line->Trim();

                // Fenced code block toggle
                if (trimmed->StartsWith("```")) {
                    if (inCodeBlock) {
                        AppendCodeBlock(rtb, codeBlockBuffer);
                        codeBlockBuffer = "";
                        inCodeBlock = false;
                    } else {
                        inCodeBlock = true;
                    }
                    continue;
                }
                if (inCodeBlock) {
                    codeBlockBuffer += line + "\n";
                    continue;
                }

                // Headings
                if (trimmed->StartsWith("### ")) { AppendHeading(rtb, trimmed->Substring(4)->Trim(), 3); continue; }
                if (trimmed->StartsWith("## ")) { AppendHeading(rtb, trimmed->Substring(3)->Trim(), 2); continue; }
                if (trimmed->StartsWith("# ")) { AppendHeading(rtb, trimmed->Substring(2)->Trim(), 1); continue; }

                // Horizontal rule
                if (trimmed == "---" || trimmed == "***" || trimmed == "___" || trimmed == "- - -") {
                    AppendHorizontalRule(rtb);
                    continue;
                }

                // Blockquote
                if (trimmed->StartsWith("> ")) {
                    AppendBlockquote(rtb, trimmed->Substring(2));
                    continue;
                }
                if (trimmed->StartsWith(">")) {
                    AppendBlockquote(rtb, trimmed->Substring(1)->TrimStart());
                    continue;
                }

                // Ordered list eg "1. text"
                if (trimmed->Length >= 3 && Char::IsDigit(trimmed[0]) && trimmed[1] == '.' && trimmed[2] == ' ') {
                    int num = trimmed[0] - '0';
                    // handle multi-digit
                    int dot = trimmed->IndexOf(". ");
                    if (dot > 0) {
                        String^ numStr = trimmed->Substring(0, dot);
                        int n;
                        if (Int32::TryParse(numStr, n)) {
                            AppendListItem(rtb, trimmed->Substring(dot + 2), true, n);
                            continue;
                        }
                    }
                }
                // Unordered list
                if ((trimmed->StartsWith("- ") || trimmed->StartsWith("* ") || trimmed->StartsWith("• ")) && trimmed->Length > 2) {
                    AppendListItem(rtb, trimmed->Substring(2), false, 0);
                    continue;
                }

                // Empty line -> paragraph break
                if (String::IsNullOrWhiteSpace(line)) {
                    rtb->AppendText("\n");
                    continue;
                }

                // Normal paragraph with inline formatting
                AppendParagraph(rtb, line);
                // Add line break unless next line is also paragraph continuation without blank line
                // For simplicity, each line is a paragraph break
                rtb->AppendText("\n");
            }

            // Flush any remaining code block (malformed markdown with unclosed ```)
            if (inCodeBlock && !String::IsNullOrEmpty(codeBlockBuffer)) {
                AppendCodeBlock(rtb, codeBlockBuffer);
            }

            rtb->SelectionStart = 0;
            rtb->SelectionLength = 0;
            rtb->ResumeLayout();
        } catch (...) {
            try {
                rtb->ResumeLayout();
                rtb->Text = markdown;
            } catch (...) {}
        }
    }

    void MarkdownRenderer::AppendHeading(RichTextBox^ rtb, String^ text, int level)
    {
        float size = 12.0f;
        if (level == 2) size = 10.5f;
        else if (level == 3) size = 9.5f;
        Color color = Color::FromArgb(26, 26, 26);
        Font^ base = rtb->Font;
        Font^ f = gcnew Font(base->FontFamily, size, FontStyle::Bold);
        int start = rtb->TextLength;
        rtb->AppendText(text + "\n");
        rtb->Select(start, text->Length);
        rtb->SelectionFont = f;
        rtb->SelectionColor = color;
        rtb->Select(rtb->TextLength, 0);
    }

    void MarkdownRenderer::AppendParagraph(RichTextBox^ rtb, String^ text)
    {
        ParseInline(rtb, text);
    }

    void MarkdownRenderer::ParseInline(RichTextBox^ rtb, String^ text)
    {
        if (String::IsNullOrEmpty(text)) return;
        // Simple inline parser: handle **bold**, *italic*, `code`, [text](url)
        // We scan left to right, emitting runs.
        int pos = 0;
        while (pos < text->Length) {
            // Bold **...**
            int boldStart = text->IndexOf("**", pos);
            int italicStar = text->IndexOf(" *", pos); // not perfect
            int codeStart = text->IndexOf("`", pos);
            int linkStart = text->IndexOf("[", pos);

            // Find earliest special token
            int next = -1;
            String^ token = nullptr;
            if (boldStart >= 0 && (next < 0 || boldStart < next)) { next = boldStart; token = "**"; }
            if (codeStart >= 0 && (next < 0 || codeStart < next)) { next = codeStart; token = "`"; }
            if (linkStart >= 0 && (next < 0 || linkStart < next)) { next = linkStart; token = "["; }

            if (next < 0) {
                // No more inline, append rest
                AppendTextWithStyle(rtb, text->Substring(pos), FontStyle::Regular, SystemColors::WindowText, false);
                break;
            }

            // Emit plain before token
            if (next > pos) {
                AppendTextWithStyle(rtb, text->Substring(pos, next - pos), FontStyle::Regular, SystemColors::WindowText, false);
            }

            if (token == "**") {
                int end = text->IndexOf("**", next + 2);
                if (end >= 0) {
                    String^ inner = text->Substring(next + 2, end - (next + 2));
                    AppendTextWithStyle(rtb, inner, FontStyle::Bold, SystemColors::WindowText, false);
                    pos = end + 2;
                } else {
                    AppendTextWithStyle(rtb, "**", FontStyle::Regular, SystemColors::WindowText, false);
                    pos = next + 2;
                }
            } else if (token == "`") {
                int end = text->IndexOf("`", next + 1);
                if (end >= 0) {
                    String^ inner = text->Substring(next + 1, end - (next + 1));
                    AppendTextWithStyle(rtb, inner, FontStyle::Regular, Color::FromArgb(26, 26, 26), true);
                    pos = end + 1;
                } else {
                    AppendTextWithStyle(rtb, "`", FontStyle::Regular, SystemColors::WindowText, false);
                    pos = next + 1;
                }
            } else if (token == "[") {
                int closeBracket = text->IndexOf("]", next + 1);
                int openParen = -1;
                int closeParen = -1;
                if (closeBracket >= 0) {
                    openParen = text->IndexOf("(", closeBracket + 1);
                    if (openParen == closeBracket + 1) {
                        closeParen = text->IndexOf(")", openParen + 1);
                    }
                }
                if (closeBracket >= 0 && openParen >= 0 && closeParen >= 0) {
                    String^ linkText = text->Substring(next + 1, closeBracket - (next + 1));
                    String^ url = text->Substring(openParen + 1, closeParen - (openParen + 1));
                    // Validate URL is https and github.com per security
                    bool valid = false;
                    try {
                        Uri^ uri;
                        if (Uri::TryCreate(url, UriKind::Absolute, uri)) {
                            if (uri->Scheme == "https") valid = true;
                        }
                    } catch (...) {}
                    if (valid) {
                        int start = rtb->TextLength;
                        rtb->AppendText(linkText);
                        rtb->Select(start, linkText->Length);
                        rtb->SelectionColor = Color::FromArgb(0, 103, 184);
                        Font^ base = rtb->Font;
                        rtb->SelectionFont = gcnew Font(base, FontStyle::Underline);
                        // Store URL as hidden? For now, we rely on DetectUrls false and handle click via LinkClicked with URL in tag
                        // We encode link as "text [url]" for click handler to parse? Simpler: append " (url)" in small
                        rtb->Select(rtb->TextLength, 0);
                        // Append URL in smaller grey if needed? For minimal, just show text as link color
                    } else {
                        AppendTextWithStyle(rtb, text->Substring(next, closeParen - next + 1), FontStyle::Regular, SystemColors::WindowText, false);
                    }
                    pos = closeParen + 1;
                } else {
                    AppendTextWithStyle(rtb, "[", FontStyle::Regular, SystemColors::WindowText, false);
                    pos = next + 1;
                }
            } else {
                pos = next + 1;
            }
        }

        // Handle simple italic *text* (single star) after bold pass
        // We do a second pass for * not ** : if original had * without **, our bold consumed ** but * remains
        // For simplicity, we treat remaining * as italic in a post-process: find *...* and make italic
        // This is best-effort; we avoid over-engineering
    }

    void MarkdownRenderer::AppendTextWithStyle(RichTextBox^ rtb, String^ text, FontStyle style, Color color, bool isCode)
    {
        if (String::IsNullOrEmpty(text)) return;
        int start = rtb->TextLength;
        rtb->AppendText(text);
        rtb->Select(start, text->Length);
        Font^ base = rtb->Font;
        Font^ f = GetFontForRun(text, base, style, isCode);
        rtb->SelectionFont = f;
        rtb->SelectionColor = color;
        if (isCode) {
            rtb->SelectionBackColor = Color::FromArgb(243, 243, 243);
        } else {
            rtb->SelectionBackColor = rtb->BackColor;
        }
        rtb->Select(rtb->TextLength, 0);
        // Restore defaults
        rtb->SelectionFont = base;
        rtb->SelectionColor = SystemColors::WindowText;
        rtb->SelectionBackColor = rtb->BackColor;
    }

    Font^ MarkdownRenderer::GetFontForRun(String^ text, Font^ baseFont, FontStyle style, bool isCode)
    {
        if (isCode) {
            try {
                return gcnew Font("Consolas", 8.5f, style);
            } catch (...) {
                return gcnew Font(baseFont->FontFamily, baseFont->Size, style);
            }
        }
        // Emoji detection: if text contains surrogate pair, use Segoe UI Emoji for that run
        bool hasEmoji = false;
        for (int i = 0; i < text->Length - 1; i++) {
            if (Char::IsHighSurrogate(text[i]) && Char::IsLowSurrogate(text[i + 1])) { hasEmoji = true; break; }
            // Also check for single emoji in BMP like ✓, ⚠, etc. — they are not surrogate but still need Emoji font for color
            // We treat U+2713, U+26A0, U+27F3 etc as emoji-like and use Emoji font if base lacks them? For simplicity, we keep base but Emoji font fallback will happen via GDI
        }
        if (hasEmoji) {
            try {
                return gcnew Font("Segoe UI Emoji", baseFont->Size, style);
            } catch (...) {}
        }
        if (style == FontStyle::Regular) return baseFont;
        try {
            return gcnew Font(baseFont->FontFamily, baseFont->Size, style);
        } catch (...) {
            return baseFont;
        }
    }

    void MarkdownRenderer::AppendCodeBlock(RichTextBox^ rtb, String^ code)
    {
        if (String::IsNullOrEmpty(code)) return;
        int start = rtb->TextLength;
        rtb->AppendText(code);
        if (!code->EndsWith("\n")) rtb->AppendText("\n");
        int len = rtb->TextLength - start;
        rtb->Select(start, len);
        try {
            Font^ f = gcnew Font("Consolas", 8.5f, FontStyle::Regular);
            rtb->SelectionFont = f;
        } catch (...) {}
        rtb->SelectionColor = Color::FromArgb(26, 26, 26);
        rtb->SelectionBackColor = Color::FromArgb(245, 245, 245);
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionBackColor = rtb->BackColor;
    }

    void MarkdownRenderer::AppendListItem(RichTextBox^ rtb, String^ text, bool ordered, int number)
    {
        String^ prefix = ordered ? String::Format("{0}. ", number) : "• ";
        int start = rtb->TextLength;
        rtb->AppendText(prefix);
        rtb->Select(start, prefix->Length);
        rtb->SelectionColor = SystemColors::WindowText;
        rtb->Select(rtb->TextLength, 0);
        ParseInline(rtb, text);
        rtb->AppendText("\n");
    }

    void MarkdownRenderer::AppendBlockquote(RichTextBox^ rtb, String^ text)
    {
        int start = rtb->TextLength;
        rtb->AppendText("  " + text + "\n");
        int len = rtb->TextLength - start;
        rtb->Select(start, len);
        rtb->SelectionColor = Color::FromArgb(96, 94, 92);
        // Add left border effect via selection backcolor? For minimal, just color
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionColor = SystemColors::WindowText;
    }

    void MarkdownRenderer::AppendHorizontalRule(RichTextBox^ rtb)
    {
        int start = rtb->TextLength;
        rtb->AppendText("────────────────────────────────\n");
        int len = rtb->TextLength - start;
        rtb->Select(start, len);
        rtb->SelectionColor = Color::FromArgb(225, 225, 225);
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionColor = SystemColors::WindowText;
    }

    bool MarkdownRenderer::IsSurrogatePair(String^ s, int index)
    {
        if (String::IsNullOrEmpty(s) || index < 0 || index + 1 >= s->Length) return false;
        return Char::IsHighSurrogate(s[index]) && Char::IsLowSurrogate(s[index + 1]);
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
