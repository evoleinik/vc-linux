"""Stdlib-only syntax readers for test_site.mjs; no generator code is imported.

Keep policy assertions in the Node gate. Python supplies TOML, HTML and XML
parsers so quoted attributes, entities, nested text and namespaces are not
mistaken for a regular-expression approximation of a document.
"""
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import sys
import tomllib
import xml.etree.ElementTree as ET


VOID = frozenset("area base br col embed hr img input link meta param source track wbr".split())
SITEMAP = "http://www.sitemaps.org/schemas/sitemap/0.9"
XHTML = "http://www.w3.org/1999/xhtml"


class Page(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.elements: list[dict] = []
        self.stack: list[dict] = []

    def handle_starttag(self, tag: str, attributes: list) -> None:
        attrs = dict(attributes)
        names = [name for name, _ in attributes]
        hidden = ("hidden" in attrs or attrs.get("aria-hidden") == "true" or
                  bool(re.search(r"(?:display\s*:\s*none|visibility\s*:\s*hidden)",
                                 attrs.get("style") or "", re.I)) or
                  any(element["hidden"] for element in self.stack))
        element = {"tag": tag, "attrs": attrs, "text": "", "hidden": hidden,
                   "ancestors": [ancestor["tag"] for ancestor in self.stack],
                   "duplicateAttributes": sorted({name for name in names if names.count(name) > 1}),
                   "startTag": self.get_starttag_text()}
        self.elements.append(element)
        if tag not in VOID:
            self.stack.append(element)

    def handle_startendtag(self, tag: str, attributes: list) -> None:
        self.handle_starttag(tag, attributes)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index]["tag"] == tag:
                del self.stack[index:]
                break

    def handle_data(self, data: str) -> None:
        for element in self.stack:
            element["text"] += data


def inspect(output: Path, config: Path) -> dict:
    result: dict = {"config": {}, "pages": {}, "files": [], "sitemap": [], "errors": []}

    def fail(page: str, field: str, message: str) -> None:
        result["errors"].append({"page": page, "field": field, "message": message})

    try:
        with config.open("rb") as stream:
            result["config"] = tomllib.load(stream)
    except (OSError, ValueError) as error:
        fail(str(config), "TOML", str(error))

    if not output.is_dir():
        fail(str(output), "publication", "directory does not exist; run make web first")
    for path in sorted(output.rglob("*")):
        relative = path.relative_to(output).as_posix()
        if not path.resolve().is_relative_to(output.resolve()):
            fail("/" + relative, "file", "symlink escapes the publication")
            continue
        if not path.is_file():
            continue
        result["files"].append(relative)
        if path.suffix.lower() != ".html":
            continue
        try:
            page = Page()
            page.feed(path.read_text(encoding="utf-8"))
            page.close()
            result["pages"][relative] = page.elements
        except (OSError, ValueError) as error:
            fail("/" + relative, "HTML", str(error))

    try:
        root = ET.parse(output / "sitemap.xml").getroot()
        if root.tag != f"{{{SITEMAP}}}urlset":
            fail("/sitemap.xml", "urlset", "missing sitemap namespace or urlset root")
        if any(element.tag.rsplit("}", 1)[-1] == "lastmod" for element in root.iter()):
            fail("/sitemap.xml", "lastmod", "build-time lastmod dates must not be emitted")
        for item in root.findall(f"{{{SITEMAP}}}url"):
            locations = item.findall(f"{{{SITEMAP}}}loc")
            result["sitemap"].append({
                "locations": [location.text or "" for location in locations],
                "alternates": [dict(link.attrib) for link in item.findall(f"{{{XHTML}}}link")],
            })
    except (OSError, ET.ParseError) as error:
        fail("/sitemap.xml", "XML", str(error))
    return result


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: .venv/bin/python tests/site_inspect.py OUTPUT CONFIG")
    print(json.dumps(inspect(Path(sys.argv[1]), Path(sys.argv[2])), ensure_ascii=False))
