#!/usr/bin/env python3
"""Narrow, verified bridge to the frozen Step 2 case-descriptor loader."""

import importlib.util
import sys
from pathlib import Path
from types import ModuleType
from typing import TypeAlias


class ReferenceInputBootstrapError(RuntimeError):
    """The frozen Step 2 projection loader could not be imported exactly."""


_PROJECTION_CONTRACT = Path(__file__).resolve().parents[1] / "projection" / "projection_contract.py"


def _load_projection_contract() -> ModuleType:
    expected = _PROJECTION_CONTRACT.resolve()
    specification = importlib.util.spec_from_file_location(
        "_ista_step2_projection_contract", expected
    )
    if specification is None or specification.loader is None:
        raise ReferenceInputBootstrapError(f"cannot load projection contract: {expected}")
    module = importlib.util.module_from_spec(specification)
    projection_directory = str(expected.parent)
    sys.path.insert(0, projection_directory)
    sys.modules[specification.name] = module
    try:
        specification.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(specification.name, None)
        raise
    finally:
        sys.path.remove(projection_directory)
    actual = Path(module.__file__ or "").resolve()
    if actual != expected:
        raise ReferenceInputBootstrapError(
            f"projection contract path mismatch: expected {expected}, got {actual}"
        )
    return module


_projection_contract = _load_projection_contract()
CaseDescriptor: TypeAlias = _projection_contract.CaseDescriptor
load_case_descriptors = _projection_contract.load_case_descriptors

__all__ = ["CaseDescriptor", "load_case_descriptors"]
