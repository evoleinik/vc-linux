"""Private, opt-in probes proving each Hack translation gate can turn red.

HACK_MUTANT=1 .venv/bin/python -m pytest -q tests/fixtures/hack-translator-mutations.py
HACK_MUTANT=0 .venv/bin/python -m pytest -q tests/fixtures/hack-translator-mutations.py

Only test-local bindings are changed; no generated C or source is overwritten.
"""

from dataclasses import replace
import os
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
import test_translator_hack as gate
import test_translator_regression as regression
from translator_support import ops_build


MUTATE = os.environ.get("HACK_MUTANT", "0") == "1"


@pytest.fixture(scope="module")
def layout():
    return gate.hack_layout.__wrapped__()


def test_complete_map_and_crt(layout):
    if MUTATE:
        layout = replace(layout, compiled_modules=())
    gate.test_hack_map_covers_game_shims_and_linked_crt(layout)


def test_callback_validation(layout, monkeypatch):
    if MUTATE:
        monkeypatch.setattr(gate, "validate_compiled_targets", lambda _: None)
    gate.test_hack_missing_callback_is_rejected_before_play(layout, "dosave_")


def test_linked_bytes_validation(layout, monkeypatch):
    if MUTATE:
        monkeypatch.setattr(gate, "verify_linked_bytes", lambda *args: None)
    gate.test_hack_linked_byte_mutation_fails_independent_omf_proof(layout, False)


def test_instruction_oracle_coverage(layout, monkeypatch):
    if MUTATE:
        cases = tuple(case for case in ops_build.load_cases() if case.image_name != "HACK.EXE")
        monkeypatch.setattr(ops_build, "load_cases", lambda: cases)
    gate.test_hack_every_distinct_instruction_is_in_unicorn_gate(layout)


def test_previous_generated_c_digest(monkeypatch):
    if MUTATE:
        original = regression.sha256
        monkeypatch.setattr(regression, "sha256", lambda data: original(data + b"\n/* defect */\n"))
    regression.test_earlier_generated_c_is_byte_identical("rogue.c")
