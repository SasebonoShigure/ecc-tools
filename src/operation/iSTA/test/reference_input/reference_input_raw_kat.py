#!/usr/bin/env python3
"""Independent hand-built complex raw-wire known-answer input."""
# ruff: noqa: FBT003

import reference_input_model as model


def complex_raw_kat_input() -> model.RawReferenceInput:
    """Return the independently hand-built nonempty raw-wire known answer."""
    missing = model.RawTimingTable(False, None, None, (), (), ())
    present_empty = model.RawTimingTable(
        True,
        model.TableVariable.NONE,
        model.TableVariable.NONE,
        (),
        (),
        (),
    )
    child = model.RawLibraryArcChild(
        "slow",
        7,
        model.TimingSense.NEGATIVE_UNATE,
        model.Transition.FALL,
        model.Transition.RISE,
        "EN",
        2.0,
        4.0,
        0.5,
        (0.125, 0.25),
        (0.75, 0.875),
        (0.25, 0.5),
        (0.5, 0.75),
        (present_empty, missing),
        (missing, missing),
        (missing, missing),
    )
    payload = model.RawLibraryCellPayload(
        True,
        False,
        False,
        "",
        "",
        "Y",
        "PF",
        "NS",
        0.5,
        (0.125, 0.25),
        (0.75, 0.875),
        (0.25, 0.5),
        (0.5, 0.75),
        (),
        (
            model.RawLibraryCellArc(
                "A",
                "Y",
                0.25,
                0.125,
                0.5,
                False,
                False,
                False,
                (child,),
            ),
        ),
        (),
    )
    z_net = model.RawParasiticNet(
        "z_net",
        "z_emb",
        1.5,
        (
            model.RawParasiticNode("z2", "z_two", 0.25, 2.0, -2.0),
            model.RawParasiticNode("z1", "z_one", 0.125, 1.0, -1.0),
        ),
        (
            model.RawParasiticResistor("z2", "z1", 8.0),
            model.RawParasiticResistor("z1", "z2", 4.0),
        ),
    )
    a_net = model.RawParasiticNet(
        "a_net",
        "a_emb",
        0.0,
        (
            model.RawParasiticNode("a2", "a_two", 0.0, 4.0, -4.0),
            model.RawParasiticNode("a1", "a_one", 0.0, 3.0, -3.0),
        ),
        (
            model.RawParasiticResistor("a2", "a1", 2.0),
            model.RawParasiticResistor("a1", "a2", 1.0),
        ),
    )
    return model.RawReferenceInput(
        (),
        (
            model.RawInstance(
                "u",
                "CELL",
                True,
                False,
                False,
                "",
                "",
                "u:Q",
                ("u:X",),
                None,
            ),
        ),
        (),
        (),
        (
            model.RawClockConstraint(
                "c",
                "e",
                ("ghost", "ghost"),
                -2.0,
                0.0,
                -1.0,
                False,
            ),
        ),
        (),
        (),
        (),
        (model.RawReferencedLibraryCell("slow", "CELL", payload),),
        model.RawParasitics("fixture.spef", "PF", "OHM", (z_net, a_net)),
    )
