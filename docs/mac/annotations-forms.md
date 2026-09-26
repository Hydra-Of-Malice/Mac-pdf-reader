# Annotations and forms on macOS

The macOS app is a viewer. It shows annotations and form fields as the document draws them, but can't create, edit,
fill or save anything. No menu item, toolbar item or dialog offers editing or saving, and the app never writes to a
document file (it only reads it, and re-reads it when it changes on disk).

## What is shown

| Content                                   | Behaviour                                                                         |
| ----------------------------------------- | --------------------------------------------------------------------------------- |
| PDF annotations (highlights, notes, ink…) | drawn from their appearance streams by MuPDF                                      |
| Annotations flagged Hidden or NoView      | not drawn on screen                                                               |
| Printing                                  | MuPDF "Print" usage: only annotations with the Print flag are printed             |
| Form fields (AcroForm widgets)            | drawn read-only with their current appearance (value as saved in the file)        |
| XFA-only forms                            | not supported by MuPDF; the static PDF content (often a "please wait" page) shows |
| Optional content (layers)                 | the document's default "View" / "Print" state; layers can't be toggled            |
| Text of free-text annotations and fields  | included in text selection, copy and search (the whole page is extracted)         |

## What doesn't work

- No annotation tools: can't add, move, edit, delete or reply to annotations; note pop-ups don't open on click.
- No form filling: clicking a field does nothing (or starts a text selection); checkboxes, radio buttons, combo boxes
  and signature fields don't respond.
- No JavaScript: field calculations, validation, formatting and JavaScript links are ignored.
- No saving: there is no Save / Save As / Export; changes can't be made, so none can be lost.
- Digital signatures aren't verified; a signature field shows its appearance only.
- Annotations aren't listed anywhere (the sidebar shows the outline and page thumbnails).

## Links

- Links to pages of the document jump there (⌘[ goes back).
- `http`, `https`, `mailto` and `ftp` links open in the default app; other URL schemes are ignored.
- Links to files: documents SumatraPDF can open open in a new tab; other files open with their default app after a
  confirmation; programs, scripts, installers and app bundles are never opened.

Implementation: `EngineMupdf::RenderPage()` (`pdf_run_page_with_usage`), `ExtractPageTextLocked()` for text, and
`MacLinkAtPoint()` / `-openLinkedFile:` in `src/mac/` for links.
