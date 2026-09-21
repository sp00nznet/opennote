"""Generate the .docx conformance corpus.

The documents are built here rather than committed, so the repository carries no
binary Office files and the corpus is reproducible from source. Each .docx gets
a matching .expect listing what the converted RTF must and must not contain;
`OpenNote.exe --docx-check <dir>` reads both and reports a pass/fail count.

Usage:
    py tests/make_fixtures.py build/corpus
"""
import os
import struct
import sys
import zipfile
import zlib

W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
WP = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
O = "urn:schemas-microsoft-com:office:office"
V = "urn:schemas-microsoft-com:vml"

CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/{target}" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
{extra}</Types>"""

# Parts that hang off the document rather than off the package: a relationship
# in word/_rels/document.xml.rels is what makes them the document's.
DOC_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
{rels}</Relationships>"""

HEADER_CT = "application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"
FOOTER_CT = "application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"
IMAGE_CT = "image/png"
STYLES_CT = "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"
NUMBERING_CT = "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"
REL_BASE = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"

RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="{target}"/>
</Relationships>"""


def p(runs, props=""):
    return f"<w:p>{props}{''.join(runs)}</w:p>"


def r(text, props=""):
    return f'<w:r>{props}<w:t xml:space="preserve">{text}</w:t></w:r>'


def document(body, sect=None):
    sect = sect or '<w:sectPr><w:pgSz w:w="12240" w:h="15840"/></w:sectPr>'
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        f'<w:document xmlns:w="{W}" xmlns:r="{R}" xmlns:wp="{WP}" xmlns:o="{O}" xmlns:v="{V}"><w:body>'
        + "".join(body)
        + sect
        + "</w:body></w:document>"
    )


def write(outdir, name, body, expect, target="word/document.xml", parts=None, sect=None):
    """parts: [(partname, content_type, rel_type, xml)], related to the document."""
    parts = parts or []
    path = os.path.join(outdir, name + ".docx")

    extra = "".join(
        '  <Override PartName="/%s" ContentType="%s"/>\n' % (part, ct)
        for part, ct, _rel, _xml in parts
    )

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES.format(target=target, extra=extra))
        z.writestr("_rels/.rels", RELS.format(target=target))
        z.writestr(target, document(body, sect))

        if parts:
            rels = ""
            for i, (part, _ct, rel, xml) in enumerate(parts, start=1):
                z.writestr(part, xml)   # bytes or str, whichever the part is
                # Relative to the document part, which is where a reader
                # resolves it from.
                # A picture is named rIdImg1 and so on, because the drawing in
                # the document refers to it by name.
                relative = "/".join(part.split("/")[1:])
                if rel == "image":
                    rid = "rIdImg%d" % i
                elif rel == "header":
                    rid = "rIdHdr"
                elif rel == "footer":
                    rid = "rIdFtr"
                else:
                    rid = "rIdX%d" % i
                rels += '  <Relationship Id="%s" Type="%s%s" Target="%s"/>\n' % (
                    rid, REL_BASE, rel, relative)

            folder, filename = target.rsplit("/", 1)
            z.writestr("%s/_rels/%s.rels" % (folder, filename), DOC_RELS.format(rels=rels))
    with open(os.path.join(outdir, name + ".expect"), "w", encoding="utf-8") as f:
        f.write("\n".join(expect) + "\n")
    return path


# --------------------------------------------------------------------------
# formatting: the character and paragraph properties people actually use
# --------------------------------------------------------------------------
def fixture_formatting(outdir):
    body = [
        p([r("A Heading")], '<w:pPr><w:pStyle w:val="Heading1"/></w:pPr>'),
        p([
            r("Plain, "),
            r("bold", "<w:rPr><w:b/></w:rPr>"),
            r(", "),
            r("italic", "<w:rPr><w:i/></w:rPr>"),
            r(", "),
            r("underline", '<w:rPr><w:u w:val="single"/></w:rPr>'),
            r(", "),
            r("struck", "<w:rPr><w:strike/></w:rPr>"),
            r(", "),
            r("coloured", '<w:rPr><w:color w:val="C81E1E"/></w:rPr>'),
            r(", "),
            r("big", '<w:rPr><w:sz w:val="36"/></w:rPr>'),
            r(", "),
            r("Courier", '<w:rPr><w:rFonts w:ascii="Courier New" w:hAnsi="Courier New"/></w:rPr>'),
            r("."),
        ]),
        p([r("Centred")], '<w:pPr><w:jc w:val="center"/></w:pPr>'),
        p([r("Righted")], '<w:pPr><w:jc w:val="right"/></w:pPr>'),
        p([r("Justified")], '<w:pPr><w:jc w:val="both"/></w:pPr>'),
        p([r("Indented")], '<w:pPr><w:ind w:left="720"/></w:pPr>'),
        p([r("Listed")],
          '<w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr>'),
        p([r("Super"), r("script", '<w:rPr><w:vertAlign w:val="superscript"/></w:rPr>'),
           r(" and sub"), r("script", '<w:rPr><w:vertAlign w:val="subscript"/></w:rPr>')]),
        p([r("Before"), "<w:r><w:tab/></w:r>", r("after")]),
    ]
    expect = [
        "# character formatting must reach the RTF",
        "contains:\\b",
        "contains:\\i",
        "contains:\\ul",
        "contains:\\strike",
        "contains:\\super",
        "contains:\\sub",
        "contains:\\tab",
        "contains:Courier New",
        "contains:\\red200\\green30\\blue30",
        "# a Heading1 style must survive into the run, not be lost when it opens",
        "contains:\\b\\fs36 A Heading",
        "# paragraph formatting",
        "contains:\\qc",
        "contains:\\qr",
        "contains:\\qj",
        "contains:\\li720",
        "# a list paragraph gets a hanging indent and a bullet",
        "contains:\\fi-360",
        "contains:pnlvlblt",
    ]
    return write(outdir, "formatting", body, expect)


# --------------------------------------------------------------------------
# tables: the grid must drive \cellx, and cells must not gain a blank line
# --------------------------------------------------------------------------
def fixture_table(outdir):
    grid = '<w:tblGrid><w:gridCol w:w="3000"/><w:gridCol w:w="2400"/><w:gridCol w:w="1600"/></w:tblGrid>'
    rows = [("Product", "Status", "Cost"),
            ("WordPad", "Removed", "n/a"),
            ("OpenNote", "Shipping", "Free")]
    trs = []
    for row in rows:
        tcs = "".join(f"<w:tc>{p([r(c)])}</w:tc>" for c in row)
        trs.append(f"<w:tr>{tcs}</w:tr>")
    body = [
        p([r("Before the table.")]),
        f"<w:tbl>{grid}{''.join(trs)}</w:tbl>",
        p([r("After the table.")]),
    ]
    expect = [
        "# cell edges come from w:tblGrid, cumulative, before the row content",
        "contains:\\cellx3000\\cellx5400\\cellx7000",
        "# cell paragraphs are marked in-table and terminated by \\cell",
        "contains:\\intbl",
        "contains:Product\\cell",
        "contains:\\row",
        "# a cell's last paragraph must not carry a paragraph mark as well",
        "absent:Product\\par",
        "contains:After the table.",
    ]
    return write(outdir, "table", body, expect)


# --------------------------------------------------------------------------
# revisions: a tracked deletion is history, not document text
# --------------------------------------------------------------------------
def fixture_revisions(outdir):
    body = [
        p([
            r("Kept before. "),
            '<w:del w:id="1" w:author="a"><w:r><w:delText>REMOVEDTEXT</w:delText></w:r></w:del>',
            r("Kept after."),
        ]),
        p([
            '<w:ins w:id="2" w:author="a"><w:r><w:t xml:space="preserve">INSERTEDTEXT</w:t></w:r></w:ins>',
        ]),
    ]
    expect = [
        "contains:Kept before.",
        "contains:Kept after.",
        "# deleted content must not appear in the document",
        "absent:REMOVEDTEXT",
        "# an accepted insertion is part of the document",
        "contains:INSERTEDTEXT",
    ]
    return write(outdir, "revisions", body, expect)


# --------------------------------------------------------------------------
# escaping: RTF control characters and non-ASCII
# --------------------------------------------------------------------------
def fixture_escaping(outdir):
    body = [
        p([r("Entities: &amp; &lt; &gt; &quot; &apos;")]),
        p([r("Braces and slash: { } \\")]),
        p([r("Unicode: em dash — café naïve 中文")]),
    ]
    expect = [
        "# RTF control characters must be escaped, not passed through",
        "contains:\\{",
        "contains:\\}",
        "contains:\\\\",
        "# non-ASCII goes out as \\uN with a fallback character",
        "contains:\\u8212?",
        "contains:\\u233?",
        "contains:\\u20013?",
        "# XML entities are decoded by the parser and must arrive as characters",
        "contains:Entities: & < > \" '",
    ]
    return write(outdir, "escaping", body, expect)


# --------------------------------------------------------------------------
# relocated: the main part is found via the relationship, not a fixed path
# --------------------------------------------------------------------------
def fixture_relocated(outdir):
    body = [p([r("Found through the package relationship.")])]
    expect = [
        "# the main part is not at /word/document.xml in this package",
        "contains:Found through the package relationship.",
    ]
    return write(outdir, "relocated", body, expect,
                 target="word/document2.xml")


# --------------------------------------------------------------------------
# styles and numbering: the two parts a document keeps its shape in
# --------------------------------------------------------------------------
def fixture_styles(outdir):
    styles = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="%s">
  <w:docDefaults>
    <w:rPrDefault><w:rPr><w:rFonts w:ascii="Cambria" w:hAnsi="Cambria"/><w:sz w:val="22"/></w:rPr></w:rPrDefault>
    <w:pPrDefault><w:pPr><w:spacing w:after="160"/></w:pPr></w:pPrDefault>
  </w:docDefaults>
  <w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>
  <w:style w:type="paragraph" w:styleId="Heading1">
    <w:name w:val="heading 1"/><w:basedOn w:val="Normal"/>
    <w:pPr><w:spacing w:before="240" w:after="120"/></w:pPr>
    <w:rPr><w:b/><w:sz w:val="36"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Quote">
    <w:name w:val="Quote"/><w:basedOn w:val="Normal"/>
    <w:pPr><w:ind w:left="720"/><w:jc w:val="center"/></w:pPr>
    <w:rPr><w:i/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="QuoteRed">
    <w:name w:val="Quote Red"/><w:basedOn w:val="Quote"/>
    <w:rPr><w:color w:val="C00000"/></w:rPr>
  </w:style>
  <w:style w:type="character" w:styleId="Strong">
    <w:name w:val="Strong"/><w:rPr><w:b/></w:rPr>
  </w:style>
</w:styles>""" % (W,)

    def lvl(i, fmt, text):
        return ('<w:lvl w:ilvl="%d"><w:start w:val="1"/><w:numFmt w:val="%s"/>'
                '<w:lvlText w:val="%s"/>'
                '<w:pPr><w:ind w:left="%d" w:hanging="360"/></w:pPr></w:lvl>'
                % (i, fmt, text, 720 + i * 360))

    numbering = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="%s">
  <w:abstractNum w:abstractNumId="7">%s</w:abstractNum>
  <w:abstractNum w:abstractNumId="8">%s</w:abstractNum>
  <w:num w:numId="1"><w:abstractNumId w:val="7"/></w:num>
  <w:num w:numId="2"><w:abstractNumId w:val="8"/></w:num>
</w:numbering>""" % (
        W,
        "".join(lvl(i, "bullet", "&#183;") for i in range(3)),
        lvl(0, "decimal", "%1.") + lvl(1, "lowerLetter", "%2)") + lvl(2, "lowerRoman", "%3."),
    )

    def item(text, num, level):
        return p([r(text)],
                 '<w:pPr><w:numPr><w:ilvl w:val="%d"/><w:numId w:val="%d"/></w:numPr></w:pPr>'
                 % (level, num))

    body = [
        p([r("Styled Heading")], '<w:pPr><w:pStyle w:val="Heading1"/></w:pPr>'),
        p([r("Body text, which takes the document defaults.")]),
        p([r("A quotation, centred and italic by style.")],
          '<w:pPr><w:pStyle w:val="Quote"/></w:pPr>'),
        p([r("A red quotation, which inherits the rest.")],
          '<w:pPr><w:pStyle w:val="QuoteRed"/></w:pPr>'),
        p([r("Plain then "), r("strong", '<w:rPr><w:rStyle w:val="Strong"/></w:rPr>'),
           r(" by character style.")]),
        item("First numbered", 2, 0),
        item("Second numbered", 2, 0),
        item("Nested letter", 2, 1),
        item("Bulleted one", 1, 0),
        item("Bulleted two", 1, 0),
    ]

    expect = [
        "# a heading resolved from styles.xml rather than guessed from its name",
        "contains:\\b\\f1\\fs36 Styled Heading",
        "# document defaults reach a paragraph that states nothing",
        "contains:Cambria",
        "# a style based on another inherits what it does not state",
        "contains:\\qc",
        "contains:\\li720",
        "contains:\\i",
        "contains:\\red192\\green0\\blue0",
        "# a character style applies to its run",
        "contains:\\b\\f1\\fs22 strong",
        "# a numbered list counts; a bulleted one does not",
        "contains:\\pndec",
        "contains:pnlvlblt",
        "contains:First numbered",
        "contains:Nested letter",
    ]

    parts = [
        ("word/styles.xml", STYLES_CT, "styles", styles),
        ("word/numbering.xml", NUMBERING_CT, "numbering", numbering),
    ]
    return write(outdir, "styles", body, expect, parts=parts)


# --------------------------------------------------------------------------
# images: DrawingML and VML, which are the two ways a picture reaches a page
# --------------------------------------------------------------------------
def png(width, height, rgb):
    """A PNG of one colour, built here so the corpus stays generated."""
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) +
            chunk(b"IEND", b""))


def fixture_images(outdir):
    drawing = (
        '<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
        '<wp:extent cx="914400" cy="457200"/>'
        '<wp:docPr id="1" name="Picture 1"/>'
        '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
        '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
        '<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">'
        '<pic:nvPicPr><pic:cNvPr id="1" name="Picture 1"/><pic:cNvPicPr/></pic:nvPicPr>'
        '<pic:blipFill><a:blip r:embed="rIdImg1"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
        '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="914400" cy="457200"/></a:xfrm>'
        '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>'
        '</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>'
    )

    vml = (
        '<w:r><w:pict>'
        '<v:shape id="_x0000_i1025" type="#_x0000_t75" style="width:36pt;height:18pt">'
        '<v:imagedata r:id="rIdImg1" o:title="Picture"/>'
        '</v:shape>'
        '</w:pict></w:r>'
    )

    body = [
        p([r("Before the picture.")]),
        "<w:p>" + drawing + "</w:p>",
        p([r("Between the two.")]),
        "<w:p>" + vml + "</w:p>",
        p([r("After the picture.")]),
    ]

    expect = [
        "# both spellings of a picture reach the view as a blip",
        "contains:\\pict",
        "# RichEdit reads \\wmetafile and drops \\pngblip without a word",
        "contains:\\wmetafile8",
        "# an inch-wide picture is 1440 twips wide, and half that tall",
        "contains:\\picwgoal1440\\pichgoal720",
        "# ...and the VML one states its size in points: 36pt is half an inch",
        "contains:\\picwgoal720\\pichgoal360",
        "# the text around them is untouched",
        "contains:Before the picture.",
        "contains:Between the two.",
        "contains:After the picture.",
    ]

    parts = [("word/media/image1.png", IMAGE_CT, "image", png(8, 4, (200, 30, 30)))]
    return write(outdir, "images", body, expect, parts=parts)


# --------------------------------------------------------------------------
# pages: the paper, the margins, the columns, and a break in the middle
# --------------------------------------------------------------------------
def fixture_pages(outdir):
    # A4 landscape, narrow margins, two columns.
    sect = (
        '<w:sectPr>'
        '<w:pgSz w:w="16838" w:h="11906" w:orient="landscape"/>'
        '<w:pgMar w:top="720" w:right="600" w:bottom="720" w:left="600"/>'
        '<w:cols w:num="2" w:space="480"/>'
        '</w:sectPr>'
    )

    body = [
        p([r("First page, first column.")]),
        p([r("Still the first section.")]),
        p([r("This paragraph starts a page of its own.")],
          '<w:pPr><w:pageBreakBefore/></w:pPr>'),
        "<w:p><w:r><w:br w:type=\"page\"/></w:r><w:r><w:t>After a break run.</w:t></w:r></w:p>",
    ]

    expect = [
        "# the text survives whatever the page is doing",
        "contains:First page, first column.",
        "contains:This paragraph starts a page of its own.",
        "contains:After a break run.",
        "# a page break reaches the view as one",
        "contains:\\page",
    ]

    return write(outdir, "pages", body, expect, sect=sect)


# --------------------------------------------------------------------------
# margins: what goes above and below the text, on every page
# --------------------------------------------------------------------------
def fixture_margins(outdir):
    def part(root, text, align=None):
        props = '<w:pPr><w:jc w:val="%s"/></w:pPr>' % align if align else ""
        return (
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            '<w:%s xmlns:w="%s" xmlns:r="%s"><w:p>%s'
            '<w:r><w:t xml:space="preserve">%s</w:t></w:r>'
            '</w:p></w:%s>' % (root, W, R, props, text, root)
        )

    sect = (
        '<w:sectPr>'
        '<w:headerReference w:type="default" r:id="rIdHdr"/>'
        '<w:footerReference w:type="default" r:id="rIdFtr"/>'
        '<w:pgSz w:w="12240" w:h="15840"/>'
        '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440"'
        ' w:header="720" w:footer="720"/>'
        '</w:sectPr>'
    )

    body = [p([r("Body text on the page itself.")])]
    for i in range(60):
        body.append(p([r("Filler paragraph %d, so the document runs to more than one page." % i)]))

    expect = [
        "# the body reaches the view; the margins do not, because the control",
        "# has no notion of a page to put them on",
        "contains:Body text on the page itself.",
        "absent:A header on every page",
    ]

    parts = [
        ("word/header1.xml", HEADER_CT, "header", part("hdr", "A header on every page")),
        ("word/footer1.xml", FOOTER_CT, "footer", part("ftr", "A footer, centred", "center")),
    ]
    return write(outdir, "margins", body, expect, parts=parts, sect=sect)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/corpus"
    os.makedirs(outdir, exist_ok=True)

    made = [
        fixture_formatting(outdir),
        fixture_table(outdir),
        fixture_revisions(outdir),
        fixture_escaping(outdir),
        fixture_relocated(outdir),
        fixture_styles(outdir),
        fixture_images(outdir),
        fixture_pages(outdir),
        fixture_margins(outdir),
    ]
    for path in made:
        print(f"  {os.path.basename(path)}  {os.path.getsize(path)} bytes")
    print(f"{len(made)} documents in {outdir}")


if __name__ == "__main__":
    main()
