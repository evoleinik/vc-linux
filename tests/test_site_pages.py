"""The static renderer preserves supplied prose and publishes inert, linked pages."""
from copy import deepcopy
from hashlib import sha256
from html import escape
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import shutil
import subprocess
import tomllib
import xml.etree.ElementTree as ET

import pytest

from tools.site_pages import (GENERATED, GOATCOUNTER, ROOT, inline_markup, publish,
                              render, run_programs)


CONFIG = ROOT / "site/pages.toml"
GOAT = '<script data-goatcounter="https://notanemulator.goatcounter.com/count" async src="./vendor/goatcounter-count.js?v=fixture-stamp"></script>'


@pytest.fixture
def document():
    with CONFIG.open("rb") as stream:
        return tomllib.load(stream)


class Scripts(HTMLParser):
    def __init__(self, markup):
        super().__init__()
        self.scripts = []
        self.current = None
        self.feed(markup)

    def handle_starttag(self, tag, attributes):
        if tag == "script":
            self.current = {"attributes": dict(attributes), "text": ""}
            self.scripts.append(self.current)

    def handle_data(self, data):
        if self.current is not None:
            self.current["text"] += data

    def handle_endtag(self, tag):
        if tag == "script":
            self.current = None


def test_all_pages_are_enumerated_reciprocally_without_build_dates(document):
    files = render(document, GOAT)
    generated = {"/" + name.removesuffix("index.html") for name in files if name.endswith(".html")}
    assert len(generated) == 2 + 2 * len(document["program"])
    namespaces = {"s": "http://www.sitemaps.org/schemas/sitemap/0.9", "x": "http://www.w3.org/1999/xhtml"}
    sitemap = ET.fromstring(files["sitemap.xml"])
    entries = sitemap.findall("s:url", namespaces)
    assert {entry.find("s:loc", namespaces).text for entry in entries} == {
        document["site"] + path for path in generated | {"/"}}
    assert "lastmod" not in files["sitemap.xml"].decode()
    for entry in entries:
        url = entry.find("s:loc", namespaces).text
        links = {link.attrib["hreflang"]: link.attrib["href"] for link in entry.findall("x:link", namespaces)}
        if url != document["site"] + "/":
            assert set(links) == {"en", "ru", "x-default"}
            assert links["x-default"] == links["en"]
            assert url in (links["en"], links["ru"])
    llms = files["llms.txt"].decode()
    assert llms.startswith("# notanemulator\n\n> " + document["hub"]["en"]["body"].split("\n\n")[0])
    for path in generated:
        if not path.startswith("/ru/"):
            assert document["site"] + path in llms
    assert "## Programs\n" in llms and "## Source\n" in llms
    assert document["repo"] in llms and document["doi"] in llms
    assert files["robots.txt"] == b"User-agent: *\nAllow: /\n\nSitemap: https://notanemulator.com/sitemap.xml\n"


def test_only_markdown_links_become_markup():
    assert inline_markup('<img src=x onerror="bad"> & **literal** [A < B](https://example.com/?a=1&b=2)') == (
        '&lt;img src=x onerror=&quot;bad&quot;&gt; &amp; **literal** '
        '<a href="https://example.com/?a=1&amp;b=2">A &lt; B</a>')
    assert inline_markup("[here](/programs/)") == '<a href="/programs/">here</a>'


@pytest.mark.parametrize("url", ["javascript:alert", "data:text/html,bad", "http://example.com/", "//example.com/", r"/\example.com"])
def test_unsafe_markdown_link_destinations_are_refused(url):
    with pytest.raises(ValueError, match="HTTPS|Unsafe"):
        inline_markup(f"[x]({url})")


def test_metadata_and_json_preserve_each_language_and_have_no_executable_page_code(document):
    files = render(document, GOAT)
    for program in document["program"]:
        for language, prefix in (("en", ""), ("ru", "ru/")):
            markup = files[f"{prefix}{program['slug']}/index.html"].decode()
            localized = program[language]
            assert f'<html lang="{language}">' in markup
            assert f"<title>{escape(localized['title'])}</title>" in markup
            assert f"<h1>{escape(localized['h1'])}</h1>" in markup
            assert '<base href="/">' in markup
            assert markup.count(GOAT) == 1
            scripts = Scripts(markup).scripts
            assert len(scripts) == 2
            assert scripts[0]["attributes"] == {"type": "application/ld+json"}
            assert scripts[1]["text"] == ""
            graph = json.loads(scripts[0]["text"])["@graph"]
            application, breadcrumb = graph
            assert application["@type"] == program["schema_type"]
            assert application["name"] == program["name"]
            # A decade such as "1990s" names no creation year, so none is claimed.
            if re.fullmatch(r"\d{4}(-\d{4})?", program["years"]):
                assert application["dateCreated"] == program["years"][:4]
            else:
                assert "dateCreated" not in application
            assert application["inLanguage"] == language
            assert application["isAccessibleForFree"] is True
            assert application["license"] == document["repo"] + "/blob/main/" + program["license_path"]
            assert [author["name"] for author in application["author"]] == program["authors"]
            for author in application["author"]:
                assert author["@type"] == ("Organization" if author["name"] in (
                    "Microsoft", "Columbia University") else "Person")
            assert breadcrumb["@type"] == "BreadcrumbList"
            assert [crumb["position"] for crumb in breadcrumb["itemListElement"]] == [1, 2, 3]
            assert breadcrumb["itemListElement"][-1]["item"] == application["url"]
            expected = "/" if program["slug"] == "volkov-commander" else f"/?run={program['slug']}"
            assert f'<a class="play" href="{expected}">{escape(document["labels"][language]["play"])}</a>' in markup
            if "also_run" in program:
                assert f'/?run={program["also_run"]["slug"]}' in markup


def test_json_data_cannot_close_its_script_element(document):
    document["program"][0]["name"] = '</script><script>alert("x")</script>'
    files = render(document, GOAT)
    scripts = Scripts(files["volkov-commander/index.html"].decode()).scripts
    assert len(scripts) == 2
    assert "\\u003c/script>" in scripts[0]["text"]
    assert json.loads(scripts[0]["text"])["@graph"][0]["name"] == document["program"][0]["name"]


def test_generated_run_map_is_frozen_and_contains_only_supplied_lines(document):
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed to execute the generated ES module")
    code = render(document, GOAT)["vc-programs.js"].decode()
    expected = run_programs(document)
    checks = ("\nimport assert from 'node:assert/strict';\n"
              f"assert.deepEqual(programs, {json.dumps(expected)});\n"
              "assert(Object.isFrozen(programs));\n"
              "for (const lines of Object.values(programs)) assert(Object.isFrozen(lines));\n"
              "assert.throws(() => programs.rogue.push('injected'));\n"
              "assert.throws(() => programs.rogue = ['injected']);\n")
    result = subprocess.run([node, "--input-type=module", "-e", code + checks],
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert expected["volkov-commander"] == []
    assert expected["volkov-commander-405"] == document["program"][0]["also_run"]["run"]


def test_publication_is_repeatable_preserves_stamps_and_copies_the_preview(tmp_path):
    site = tmp_path / "site"
    site.mkdir()
    (site / "index.html").write_text(GOAT)
    assert publish(CONFIG, site) == 18
    before = {path.relative_to(site): (path.read_bytes(), path.stat().st_mtime_ns)
              for path in site.rglob("*") if path.is_file()}
    assert publish(CONFIG, site) == 18
    assert before == {path.relative_to(site): (path.read_bytes(), path.stat().st_mtime_ns)
                      for path in site.rglob("*") if path.is_file()}
    assert (site / "social-preview.png").read_bytes() == (ROOT / "docs/social-preview.png").read_bytes()
    assert GOATCOUNTER.findall((site / "ru/rogue/index.html").read_text()) == [GOAT]
    assert (site / "index.html").read_text() == GOAT


def test_stale_generated_pages_are_retired_recoverably_only(tmp_path):
    site = tmp_path / "site"
    old = site / "retired/index.html"
    old.parent.mkdir(parents=True)
    content = ("<!doctype html>\n" + GENERATED + "obsolete output").encode()
    old.write_bytes(content)
    user = site / "user/index.html"
    user.parent.mkdir()
    user.write_bytes(b"user-owned")
    publish(CONFIG, site)
    assert not old.exists()
    assert user.read_bytes() == b"user-owned"
    assert [path.read_bytes() for path in (tmp_path / "site-retired-pages").rglob("index.html.*")] == [content]


def test_symlinked_output_directory_is_refused_before_any_publication(tmp_path):
    site, untouched = tmp_path / "site", tmp_path / "untouched"
    site.mkdir()
    untouched.mkdir()
    (site / "rogue").symlink_to(untouched, target_is_directory=True)
    with pytest.raises(ValueError, match="symlink"):
        publish(CONFIG, site)
    assert not list(untouched.iterdir())
    assert not (site / "programs").exists()


@pytest.mark.parametrize("symlink", [False, True])
def test_retirement_never_overwrites_a_user_file_or_follows_a_symlink(tmp_path, symlink):
    site = tmp_path / "site"
    old = site / "obsolete/index.html"
    old.parent.mkdir(parents=True)
    content = ("<!doctype html>\n" + GENERATED + "obsolete output").encode()
    old.write_bytes(content)
    recovery = tmp_path / "site-retired-pages/obsolete"
    if symlink:
        target = tmp_path / "untouched"
        target.mkdir()
        recovery.parent.mkdir()
        recovery.symlink_to(target, target_is_directory=True)
    else:
        recovery.mkdir(parents=True)
        target = recovery
    victim = target / ("index.html." + sha256(content).hexdigest())
    victim.write_bytes(b"user-owned")
    with pytest.raises(ValueError, match="symlink|retired page"):
        publish(CONFIG, site)
    assert old.read_bytes() == content
    assert victim.read_bytes() == b"user-owned"
    assert not (site / "programs").exists()


@pytest.mark.parametrize("slug", ["../outside", "/absolute", "ru", "programs"])
def test_generated_paths_cannot_escape_the_site_or_replace_the_hubs(document, slug):
    invalid = deepcopy(document)
    invalid["program"][0]["slug"] = slug
    with pytest.raises(ValueError, match="slug"):
        render(invalid, GOAT)
