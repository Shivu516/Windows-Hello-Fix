#include "HtmlRenderer.h"

using namespace System;
using namespace System::Drawing;
using namespace System::Windows::Forms;
using namespace System::Text;
using namespace System::Text::RegularExpressions;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    String^ HtmlRenderer::EscapeHtml(String^ text)
    {
        if (String::IsNullOrEmpty(text)) return "";
        return text->Replace("&", "&amp;")->Replace("<", "&lt;")->Replace(">", "&gt;")->Replace("\"", "&quot;");
    }

    String^ HtmlRenderer::UnescapeHtml(String^ text)
    {
        if (String::IsNullOrEmpty(text)) return "";
        return text->Replace("&lt;", "<")->Replace("&gt;", ">")->Replace("&amp;", "&")->Replace("&quot;", "\"")->Replace("&#39;", "'");
    }

    bool HtmlRenderer::IsSafeUrl(String^ url)
    {
        if (String::IsNullOrEmpty(url)) return false;
        String^ t = url->Trim();
        if (t->StartsWith("https://", StringComparison::OrdinalIgnoreCase)) return true;
        if (t->StartsWith("http://", StringComparison::OrdinalIgnoreCase)) return true;
        return false;
    }

    String^ HtmlRenderer::SanitizeHtml(String^ html)
    {
        if (String::IsNullOrEmpty(html)) return "";
        String^ s = html;
        // Strip dangerous tags (case-insensitive)
        array<String^>^ dangerous = gcnew array<String^>{ "script", "iframe", "object", "embed", "style", "link", "meta", "base", "form", "input", "button", "svg", "math", "canvas" };
        for each (String^ tag in dangerous) {
            // Remove <tag ...>...</tag> and <tag .../>
            String^ patternOpen = String::Format("<{0}[^>]*>", tag);
            String^ patternClose = String::Format("</{0}[^>]*>", tag);
            try {
                s = Regex::Replace(s, patternOpen, "", RegexOptions::IgnoreCase);
                s = Regex::Replace(s, patternClose, "", RegexOptions::IgnoreCase);
            } catch (...) {}
        }
        // Strip event handlers: onload=, onclick=, onerror= etc
        try {
            s = Regex::Replace(s, "on\\w+\\s*=\\s*\"[^\"]*\"", "", RegexOptions::IgnoreCase);
            s = Regex::Replace(s, "on\\w+\\s*=\\s*'[^']*'", "", RegexOptions::IgnoreCase);
            s = Regex::Replace(s, "on\\w+\\s*=\\s*[^\\s>]+", "", RegexOptions::IgnoreCase);
        } catch (...) {}
        // Sanitize href="javascript:" etc — allow only http/https
        try {
            // Find all href="..." and validate
            Regex^ hrefRegex = gcnew Regex("href\\s*=\\s*\"([^\"]*)\"", RegexOptions::IgnoreCase);
            MatchCollection^ matches = hrefRegex->Matches(s);
            // Iterate backwards to not mess indices
            for (int i = matches->Count - 1; i >= 0; i--) {
                Match^ m = matches[i];
                String^ url = m->Groups[1]->Value;
                if (!IsSafeUrl(url)) {
                    s = s->Remove(m->Index, m->Length);
                    s = s->Insert(m->Index, "href=\"#\"");
                }
            }
            Regex^ hrefSingle = gcnew Regex("href\\s*=\\s*'([^']*)'", RegexOptions::IgnoreCase);
            matches = hrefSingle->Matches(s);
            for (int i = matches->Count - 1; i >= 0; i--) {
                Match^ m = matches[i];
                String^ url = m->Groups[1]->Value;
                if (!IsSafeUrl(url)) {
                    s = s->Remove(m->Index, m->Length);
                    s = s->Insert(m->Index, "href='#'");
                }
            }
        } catch (...) {}
        // Also handle data: and vbscript: in href
        try {
            s = Regex::Replace(s, "href\\s*=\\s*\"\\s*data:[^\"]*\"", "href=\"#\"", RegexOptions::IgnoreCase);
            s = Regex::Replace(s, "href\\s*=\\s*'\\s*data:[^']*'", "href='#'", RegexOptions::IgnoreCase);
            s = Regex::Replace(s, "href\\s*=\\s*\"\\s*javascript:[^\"]*\"", "href=\"#\"", RegexOptions::IgnoreCase);
            s = Regex::Replace(s, "href\\s*=\\s*\"\\s*vbscript:[^\"]*\"", "href=\"#\"", RegexOptions::IgnoreCase);
        } catch (...) {}
        return s;
    }

    String^ HtmlRenderer::MarkdownToHtml(String^ markdown)
    {
        if (String::IsNullOrWhiteSpace(markdown)) return "<p>No release notes.</p>";
        String^ normalized = markdown->Replace("\r\n", "\n")->Replace("\r", "\n");
        array<String^>^ lines = normalized->Split('\n');
        StringBuilder^ html = gcnew StringBuilder();
        bool inCodeBlock = false;
        StringBuilder^ codeBuffer = gcnew StringBuilder();
        bool inList = false;
        String^ listType = nullptr; // "ul" or "ol"

        for (int i = 0; i < lines->Length; i++) {
            String^ line = lines[i];
            String^ trimmed = line->Trim();

            // Fenced code block toggle
            if (trimmed->StartsWith("```")) {
                if (inCodeBlock) {
                    // Close code block
                    String^ code = codeBuffer->ToString();
                    html->Append("<pre><code>");
                    html->Append(EscapeHtml(code));
                    html->Append("</code></pre>\n");
                    codeBuffer->Clear();
                    inCodeBlock = false;
                } else {
                    if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                    inCodeBlock = true;
                }
                continue;
            }
            if (inCodeBlock) {
                codeBuffer->Append(line);
                codeBuffer->Append("\n");
                continue;
            }

            // Headings
            if (trimmed->StartsWith("### ")) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                html->AppendFormat("<h3>{0}</h3>\n", EscapeHtml(trimmed->Substring(4)->Trim()));
                continue;
            }
            if (trimmed->StartsWith("## ")) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                html->AppendFormat("<h2>{0}</h2>\n", EscapeHtml(trimmed->Substring(3)->Trim()));
                continue;
            }
            if (trimmed->StartsWith("# ")) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                html->AppendFormat("<h1>{0}</h1>\n", EscapeHtml(trimmed->Substring(2)->Trim()));
                continue;
            }

            // Horizontal rule
            if (trimmed == "---" || trimmed == "***" || trimmed == "___" || trimmed == "- - -") {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                html->Append("<hr/>\n");
                continue;
            }

            // Blockquote
            if (trimmed->StartsWith("> ")) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                String^ inner = trimmed->Substring(2);
                // Process inline markdown inside blockquote
                html->Append("<blockquote>");
                // Inline: handle **, *, `, [text](url) via placeholders, but we escape first then replace
                String^ esc = EscapeHtml(inner);
                // Bold
                esc = Regex::Replace(esc, "\\*\\*(.+?)\\*\\*", "<strong>$1</strong>");
                esc = Regex::Replace(esc, "__(.+?)__", "<strong>$1</strong>");
                // Italic (after bold)
                esc = Regex::Replace(esc, "\\*(.+?)\\*", "<em>$1</em>");
                esc = Regex::Replace(esc, "_([^_]+?)_", "<em>$1</em>");
                // Inline code
                esc = Regex::Replace(esc, "`([^`]+?)`", "<code>$1</code>");
                // Links [text](url)
                esc = Regex::Replace(esc, "\\[([^\\]]+)\\]\\((https?://[^\\s\\)]+)\\)", "<a href=\"$2\">$1</a>");
                html->Append(esc);
                html->Append("</blockquote>\n");
                continue;
            }
            if (trimmed->StartsWith(">")) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                String^ inner = trimmed->Substring(1)->TrimStart();
                String^ esc = EscapeHtml(inner);
                esc = Regex::Replace(esc, "\\*\\*(.+?)\\*\\*", "<strong>$1</strong>");
                esc = Regex::Replace(esc, "`([^`]+?)`", "<code>$1</code>");
                html->AppendFormat("<blockquote>{0}</blockquote>\n", esc);
                continue;
            }

            // Ordered list
            int dot = trimmed->IndexOf(". ");
            if (dot > 0) {
                String^ numStr = trimmed->Substring(0, dot);
                int n;
                if (Int32::TryParse(numStr, n) && trimmed->Length > dot + 2) {
                    if (!inList || listType != "ol") { if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; } html->Append("<ol>\n"); inList = true; listType = "ol"; }
                    String^ item = trimmed->Substring(dot + 2);
                    String^ esc = EscapeHtml(item);
                    esc = Regex::Replace(esc, "\\*\\*(.+?)\\*\\*", "<strong>$1</strong>");
                    esc = Regex::Replace(esc, "`([^`]+?)`", "<code>$1</code>");
                    esc = Regex::Replace(esc, "\\[([^\\]]+)\\]\\((https?://[^\\s\\)]+)\\)", "<a href=\"$2\">$1</a>");
                    html->AppendFormat("<li>{0}</li>\n", esc);
                    continue;
                }
            }
            // Unordered list
            if ((trimmed->StartsWith("- ") || trimmed->StartsWith("* ")) && trimmed->Length > 2) {
                if (!inList || listType != "ul") { if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; } html->Append("<ul>\n"); inList = true; listType = "ul"; }
                String^ item = trimmed->Substring(2);
                String^ esc = EscapeHtml(item);
                esc = Regex::Replace(esc, "\\*\\*(.+?)\\*\\*", "<strong>$1</strong>");
                esc = Regex::Replace(esc, "`([^`]+?)`", "<code>$1</code>");
                esc = Regex::Replace(esc, "\\[([^\\]]+)\\]\\((https?://[^\\s\\)]+)\\)", "<a href=\"$2\">$1</a>");
                html->AppendFormat("<li>{0}</li>\n", esc);
                continue;
            }
            if (trimmed->StartsWith("* ") && trimmed->Length > 2) {
                if (!inList || listType != "ul") { if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; } html->Append("<ul>\n"); inList = true; listType = "ul"; }
                String^ item = trimmed->Substring(2);
                html->AppendFormat("<li>{0}</li>\n", EscapeHtml(item));
                continue;
            }

            // Empty line -> close list and add paragraph break
            if (String::IsNullOrWhiteSpace(line)) {
                if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
                continue;
            }

            // Normal paragraph (close list first)
            if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
            {
                String^ esc = EscapeHtml(line->Trim());
                // Inline formatting for paragraph
                esc = Regex::Replace(esc, "\\*\\*(.+?)\\*\\*", "<strong>$1</strong>");
                esc = Regex::Replace(esc, "__(.+?)__", "<strong>$1</strong>");
                esc = Regex::Replace(esc, "\\*(.+?)\\*", "<em>$1</em>");
                esc = Regex::Replace(esc, "_([^_]+?)_", "<em>$1</em>");
                esc = Regex::Replace(esc, "`([^`]+?)`", "<code>$1</code>");
                esc = Regex::Replace(esc, "\\[([^\\]]+)\\]\\((https?://[^\\s\\)]+)\\)", "<a href=\"$2\">$1</a>");
                html->AppendFormat("<p>{0}</p>\n", esc);
            }
        }

        if (inList) { html->AppendFormat("</{0}>\n", listType); inList = false; listType = nullptr; }
        // Flush any remaining code block (malformed markdown with unclosed ```)
        if (inCodeBlock && codeBuffer->Length > 0) {
            html->Append("<pre><code>");
            html->Append(EscapeHtml(codeBuffer->ToString()));
            html->Append("</code></pre>\n");
        }

        return html->ToString();
    }

    void HtmlRenderer::Render(RichTextBox^ rtb, String^ markdown)
    {
        if (rtb == nullptr) return;
        try {
            String^ html = MarkdownToHtml(markdown);
            String^ safe = SanitizeHtml(html);
            HtmlToRtf(rtb, safe);
        } catch (...) {
            try {
                rtb->Clear();
                rtb->AppendText(markdown != nullptr ? markdown : "");
            } catch (...) {}
        }
    }

    void HtmlRenderer::HtmlToRtf(RichTextBox^ rtb, String^ html)
    {
        if (rtb == nullptr) return;
        try {
            rtb->SuspendLayout();
            rtb->Clear();
            rtb->ReadOnly = true;
            rtb->WordWrap = true;
            rtb->DetectUrls = false;
            rtb->BackColor = SystemColors::Window;
            rtb->ForeColor = SystemColors::WindowText;

            if (String::IsNullOrWhiteSpace(html)) {
                rtb->SelectionColor = Color::FromArgb(96, 94, 92);
                rtb->SelectedText = "No release notes.";
                rtb->ResumeLayout();
                return;
            }

            // Simple HTML parser: split by tags, handle allow-list
            // We use Regex to find tags
            Regex^ tagRegex = gcnew Regex("<(/?)(\\w+)([^>]*)>", RegexOptions::IgnoreCase);
            int pos = 0;
            MatchCollection^ matches = tagRegex->Matches(html);
            String^ textBuffer = "";
            bool inStrong = false, inEm = false, inCodeInline = false, inPre = false, inA = false, inBlockquote = false;
            String^ currentHref = nullptr;
            bool inLi = false;
            bool inH1 = false, inH2 = false, inH3 = false;

            // flushText inlined

            for each (Match^ m in matches) {
                // Text before tag
                if (m->Index > pos) {
                    String^ txt = html->Substring(pos, m->Index - pos);
                    // Decode entities
                    txt = txt->Replace("&lt;", "<")->Replace("&gt;", ">")->Replace("&amp;", "&");
                    // Only emit if not just whitespace between tags
                    if (!String::IsNullOrWhiteSpace(txt)) {
                        // If inside list, we already handle via li
                        {
                            String^ _txt2 = txt;
                            if (!String::IsNullOrEmpty(_txt2)) {
                                _txt2 = _txt2->Replace("&lt;", "<")->Replace("&gt;", ">")->Replace("&amp;", "&")->Replace("&quot;", "\"");
                                FontStyle _style2 = FontStyle::Regular;
                                if (inStrong) _style2 = (FontStyle)(_style2 | FontStyle::Bold);
                                if (inEm) _style2 = (FontStyle)(_style2 | FontStyle::Italic);
                                bool _isCode2 = inCodeInline || inPre;
                                Color _color2 = SystemColors::WindowText;
                                if (inH1 || inH2 || inH3) _color2 = Color::FromArgb(26, 26, 26);
                                else if (inBlockquote) _color2 = Color::FromArgb(96, 94, 92);
                                else if (inA) _color2 = Color::FromArgb(0, 103, 184);
                                if (_isCode2) AppendTextWithStyle(rtb, _txt2, _style2, Color::FromArgb(26, 26, 26), true);
                                else if (inA) { int _start2 = rtb->TextLength; rtb->AppendText(_txt2); rtb->Select(_start2, _txt2->Length); Font^ _base2 = rtb->Font; Font^ _f2 = gcnew Font(_base2, (FontStyle)(_style2 | FontStyle::Underline)); rtb->SelectionFont = _f2; rtb->SelectionColor = Color::FromArgb(0, 103, 184); rtb->Select(rtb->TextLength, 0); }
                                else AppendTextWithStyle(rtb, _txt2, _style2, _color2, false);
                            }
                        }
                    }
                }
                String^ slash = m->Groups[1]->Value;
                String^ tag = m->Groups[2]->Value->ToLowerInvariant();
                String^ attrs = m->Groups[3]->Value;
                bool isClosing = slash == "/";

                if (!isClosing) {
                    if (tag == "h1") { inH1 = true; }
                    else if (tag == "h2") { inH2 = true; }
                    else if (tag == "h3") { inH3 = true; }
                    else if (tag == "strong" || tag == "b") { inStrong = true; }
                    else if (tag == "em" || tag == "i") { inEm = true; }
                    else if (tag == "code" && !inPre) { inCodeInline = true; }
                    else if (tag == "pre") { inPre = true; rtb->AppendText("\n"); }
                    else if (tag == "a") {
                        inA = true;
                        // Extract href
                        Match^ hrefMatch = Regex::Match(attrs, "href\\s*=\\s*\"([^\"]*)\"", RegexOptions::IgnoreCase);
                        if (hrefMatch->Success) currentHref = hrefMatch->Groups[1]->Value;
                        else {
                            hrefMatch = Regex::Match(attrs, "href\\s*=\\s*'([^']*)'", RegexOptions::IgnoreCase);
                            if (hrefMatch->Success) currentHref = hrefMatch->Groups[1]->Value;
                        }
                    }
                    else if (tag == "blockquote") { inBlockquote = true; }
                    else if (tag == "li") { inLi = true; rtb->AppendText("  * "); }
                    else if (tag == "hr") { AppendHorizontalRule(rtb); }
                    else if (tag == "br") { rtb->AppendText("\n"); }
                    else if (tag == "p") { /* start paragraph */ }
                    else if (tag == "ul" || tag == "ol") { /* list container */ }
                } else {
                    if (tag == "h1") {
                        // End heading - add newline and reset style
                        rtb->AppendText("\n");
                        inH1 = false;
                    }
                    else if (tag == "h2") { rtb->AppendText("\n"); inH2 = false; }
                    else if (tag == "h3") { rtb->AppendText("\n"); inH3 = false; }
                    else if (tag == "strong" || tag == "b") { inStrong = false; }
                    else if (tag == "em" || tag == "i") { inEm = false; }
                    else if (tag == "code" && !inPre) { inCodeInline = false; }
                    else if (tag == "pre") { inPre = false; rtb->AppendText("\n"); }
                    else if (tag == "a") { inA = false; currentHref = nullptr; }
                    else if (tag == "blockquote") { inBlockquote = false; rtb->AppendText("\n"); }
                    else if (tag == "li") { inLi = false; rtb->AppendText("\n"); }
                    else if (tag == "p") { rtb->AppendText("\n"); }
                    else if (tag == "ul" || tag == "ol") { rtb->AppendText("\n"); }
                }
                pos = m->Index + m->Length;
            }
            // Trailing text
            if (pos < html->Length) {
                String^ txt = html->Substring(pos);
                txt = txt->Replace("&lt;", "<")->Replace("&gt;", ">")->Replace("&amp;", "&");
                if (!String::IsNullOrWhiteSpace(txt)) {
                            String^ _txt2 = txt;
                            _txt2 = _txt2->Replace("&lt;", "<")->Replace("&gt;", ">")->Replace("&amp;", "&")->Replace("&quot;", "\"");
                            FontStyle _style2 = FontStyle::Regular;
                            if (inStrong) _style2 = (FontStyle)(_style2 | FontStyle::Bold);
                            if (inEm) _style2 = (FontStyle)(_style2 | FontStyle::Italic);
                            bool _isCode2 = inCodeInline || inPre;
                            Color _color2 = SystemColors::WindowText;
                            if (inH1 || inH2 || inH3) _color2 = Color::FromArgb(26, 26, 26);
                            else if (inBlockquote) _color2 = Color::FromArgb(96, 94, 92);
                            else if (inA) _color2 = Color::FromArgb(0, 103, 184);
                            if (_isCode2) AppendTextWithStyle(rtb, _txt2, _style2, Color::FromArgb(26, 26, 26), true);
                            else if (inA) { int _start2 = rtb->TextLength; rtb->AppendText(_txt2); rtb->Select(_start2, _txt2->Length); Font^ _base2 = rtb->Font; Font^ _f2 = gcnew Font(_base2, (FontStyle)(_style2 | FontStyle::Underline)); rtb->SelectionFont = _f2; rtb->SelectionColor = Color::FromArgb(0, 103, 184); rtb->Select(rtb->TextLength, 0); }
                            else AppendTextWithStyle(rtb, _txt2, _style2, _color2, false);
                        }
            }

            rtb->SelectionStart = 0;
            rtb->SelectionLength = 0;
            rtb->ResumeLayout();
        } catch (...) {
            try { rtb->ResumeLayout(); rtb->Text = UnescapeHtml(html); } catch (...) {}
        }
    }

    void HtmlRenderer::AppendHeading(RichTextBox^ rtb, String^ text, int level)
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

    void HtmlRenderer::AppendTextWithStyle(RichTextBox^ rtb, String^ text, FontStyle style, Color color, bool isCode)
    {
        if (String::IsNullOrEmpty(text)) return;
        int start = rtb->TextLength;
        rtb->AppendText(text);
        rtb->Select(start, text->Length);
        Font^ base = rtb->Font;
        Font^ f = GetFontForRun(text, base, style, isCode);
        rtb->SelectionFont = f;
        rtb->SelectionColor = color;
        if (isCode) rtb->SelectionBackColor = Color::FromArgb(243, 243, 243);
        else rtb->SelectionBackColor = rtb->BackColor;
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionFont = base;
        rtb->SelectionColor = SystemColors::WindowText;
        rtb->SelectionBackColor = rtb->BackColor;
    }

    Font^ HtmlRenderer::GetFontForRun(String^ text, Font^ baseFont, FontStyle style, bool isCode)
    {
        if (isCode) {
            try { return gcnew Font("Consolas", 8.5f, style); } catch (...) { return gcnew Font(baseFont->FontFamily, baseFont->Size, style); }
        }
        bool hasEmoji = false;
        for (int i = 0; i < text->Length - 1; i++) {
            if (Char::IsHighSurrogate(text[i]) && Char::IsLowSurrogate(text[i + 1])) { hasEmoji = true; break; }
        }
        if (hasEmoji) {
            try { return gcnew Font("Segoe UI Emoji", baseFont->Size, style); } catch (...) {}
        }
        if (style == FontStyle::Regular) return baseFont;
        try { return gcnew Font(baseFont->FontFamily, baseFont->Size, style); } catch (...) { return baseFont; }
    }

    void HtmlRenderer::AppendCodeBlock(RichTextBox^ rtb, String^ code)
    {
        if (String::IsNullOrEmpty(code)) return;
        int start = rtb->TextLength;
        rtb->AppendText(code);
        if (!code->EndsWith("\n")) rtb->AppendText("\n");
        int len = rtb->TextLength - start;
        rtb->Select(start, len);
        try { Font^ f = gcnew Font("Consolas", 8.5f, FontStyle::Regular); rtb->SelectionFont = f; } catch (...) {}
        rtb->SelectionColor = Color::FromArgb(26, 26, 26);
        rtb->SelectionBackColor = Color::FromArgb(245, 245, 245);
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionBackColor = rtb->BackColor;
    }

    void HtmlRenderer::AppendHorizontalRule(RichTextBox^ rtb)
    {
        int start = rtb->TextLength;
        rtb->AppendText("────────────────────────────────\n");
        int len = rtb->TextLength - start;
        rtb->Select(start, len);
        rtb->SelectionColor = Color::FromArgb(225, 225, 225);
        rtb->Select(rtb->TextLength, 0);
        rtb->SelectionColor = SystemColors::WindowText;
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
