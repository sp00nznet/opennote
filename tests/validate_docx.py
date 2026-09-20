"""Validate the .docx files OpenNote's writer produced.

Checked with tools that are not OpenNote -- Python's zipfile and XML parser --
so a package that only OpenNote's own reader accepts does not pass.

Usage:
    py tests/validate_docx.py build/corpus
"""
import glob
import sys
import zipfile
import xml.etree.ElementTree as ET

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
CT = "{http://schemas.openxmlformats.org/package/2006/content-types}"
PR = "{http://schemas.openxmlformats.org/package/2006/relationships}"

failures = 0
checked = 0

# Everything the writer produced: .out.docx came through the editor,
# .model.docx straight from the serializer. Both are packages a reader
# other than OpenNote has to accept.
for path in sorted(glob.glob(sys.argv[1] + "/*.docx")):
    name = path.split("\\")[-1].split("/")[-1]

    def bad(msg):
        global failures
        print(f"  FAIL {name}: {msg}")
        failures += 1

    try:
        z = zipfile.ZipFile(path)
    except Exception as e:
        bad(f"not a readable zip: {e}")
        continue

    checked += 1
    names = z.namelist()

    bad_zip = z.testzip()
    if bad_zip:
        bad(f"corrupt entry {bad_zip}")

    if "[Content_Types].xml" not in names:
        bad("no [Content_Types].xml")
        continue
    if "_rels/.rels" not in names:
        bad("no package relationships part")
        continue

    # Content types must declare the main document part.
    ct = ET.fromstring(z.read("[Content_Types].xml"))
    overrides = {o.get("PartName"): o.get("ContentType")
                 for o in ct.findall(CT + "Override")}
    # OPC resolves a part's content type from an Override if one exists, and
    # otherwise from the Default for its extension. Both are valid; requiring an
    # Override would reject packages the specification allows.
    defaults = {d.get("Extension", "").lower(): d.get("ContentType")
                for d in ct.findall(CT + "Default")}

    def content_type_of(part):
        if "/" + part in overrides:
            return overrides["/" + part]
        ext = part.rsplit(".", 1)[-1].lower() if "." in part else ""
        return defaults.get(ext)

    # The package relationship must point at a part that exists.
    rels = ET.fromstring(z.read("_rels/.rels"))
    main = None
    for rel in rels.findall(PR + "Relationship"):
        if rel.get("Type", "").endswith("/officeDocument"):
            main = rel.get("Target").lstrip("/")
    if not main:
        bad("no officeDocument relationship")
        continue
    if main not in names:
        bad(f"relationship points at {main}, which is not in the package")
        continue
    ctype = content_type_of(main)
    if ctype is None:
        bad(f"/{main} has no resolvable content type")
    elif not ctype.endswith("wordprocessingml.document.main+xml"):
        bad(f"/{main} has content type {ctype}")

    # The document part must be well-formed XML with a body.
    try:
        doc = ET.fromstring(z.read(main))
    except Exception as e:
        bad(f"{main} is not well-formed XML: {e}")
        continue

    if doc.tag != W + "document":
        bad(f"root element is {doc.tag}, expected w:document")
    body = doc.find(W + "body")
    if body is None:
        bad("no w:body")
        continue
    if body.find(W + "sectPr") is None:
        bad("no w:sectPr -- Word requires a section")

    paras = body.findall(W + "p")
    if not paras:
        bad("no paragraphs")

    # No control characters may have reached the document text.
    for t in doc.iter(W + "t"):
        if t.text:
            for ch in t.text:
                if ord(ch) < 0x20 and ch != "\t":
                    bad(f"control character U+{ord(ch):04X} in document text")
                    break
                if 0xFFF9 <= ord(ch) <= 0xFFFC:
                    bad(f"RichEdit structure character U+{ord(ch):04X} in document text")
                    break

    print(f"  ok   {name}  ({len(paras)} paragraphs, {len(names)} parts)")

print(f"\n{checked - failures}/{checked} written documents valid")
sys.exit(1 if failures else 0)
