"""Offline OpenAPI 3.1 and schema checks; tooling is not a product dependency."""

import argparse
from copy import deepcopy
import json
from pathlib import Path

from jsonschema import Draft202012Validator
from openapi_spec_validator import OpenAPIV31SpecValidator


def validate_schema(schema):
    Draft202012Validator.check_schema(schema)


def validate_boundaries(path):
    def load(filename):
        schema = json.loads(Path(filename).read_text(encoding="utf-8"))
        validate_schema(schema)
        return Draft202012Validator(schema)

    incoming = load(path)
    outgoing = load(str(path) + ".output.json")
    numeric_enum = load(str(path) + ".uint64-enum.json")
    boolean_enum = load(str(path) + ".bool-enum.json")
    maximum = (1 << 64) - 1
    minimum = 9007199254740993
    assert incoming.schema["properties"]["external-id"]["minimum"] == minimum
    assert incoming.schema["properties"]["external-id"]["maximum"] == maximum
    valid = {"display\"name": "Ada", "external-id": maximum, "age": None,
             "entries": [{"text": "\U0001f600"}], "fixed": [1, 2],
             "attributes": {"first": None}, "mode": None}
    checks = 0

    def check(validator, value, expected, label):
        nonlocal checks
        errors = list(validator.iter_errors(value))
        assert (not errors) == expected, f"{label}: {[str(error) for error in errors]}"
        checks += 1

    check(incoming, valid, True, "valid input")
    check(outgoing, valid, True, "valid output")
    for name, value, expected in [
        ("external-id", minimum, True), ("external-id", minimum - 1, False),
        ("external-id", maximum + 1, False), ("external-id", str(maximum), False),
        ("age", 18, True), ("age", 120, True), ("age", 17, False), ("age", 121, False),
        ("display\"name", "\U0001f600" * 40, True), ("display\"name", "\U0001f600" * 41, False),
        ("display\"name", "", False),
        ("entries", [{"text": "\U0001f600" * 5}], True),
        ("entries", [{"text": "\U0001f600" * 6}], False),
        ("entries", [], False), ("entries", [{"text": "x"}] * 3, True),
        ("entries", [{"text": "x"}] * 4, False),
        ("fixed", [1], False), ("fixed", [1, 2, 3], False),
        ("attributes", {}, False), ("attributes", {str(i): None for i in range(4)}, True),
        ("attributes", {str(i): None for i in range(5)}, False),
        ("mode", "active", True), ("mode", "disabled", True),
        ("mode", "unknown", False), ("mode", 0, False), ("mode", False, False),
    ]:
        candidate = deepcopy(valid)
        candidate[name] = value
        check(incoming, candidate, expected, name)
    for name in ["age", "mode"]:
        candidate = deepcopy(valid)
        del candidate[name]
        check(incoming, candidate, True, f"missing optional input {name}")
        check(outgoing, candidate, False, f"missing nullable output {name}")
    candidate = deepcopy(valid)
    del candidate["external-id"]
    check(incoming, candidate, False, "missing required input")
    for value, expected in [(minimum, True), (maximum, True), (minimum - 1, False),
                            (maximum + 1, False), (None, False), (False, False)]:
        check(numeric_enum, value, expected, "uint64 enum")
    for value, expected in [(True, True), (False, True), (None, True), (0, False),
                            (1, False), ("true", False)]:
        check(boolean_enum, value, expected, "nullable bool enum")
    print(f"JSON Schema 2020-12 GO: {checks} positive/negative boundary checks")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("document", type=Path)
    parser.add_argument("--served", type=Path)
    parser.add_argument("--schemas", type=Path)
    args = parser.parse_args()
    document = json.loads(args.document.read_text(encoding="utf-8"))
    assert document["openapi"] == "3.1.0"
    errors = list(OpenAPIV31SpecValidator(document).iter_errors())
    assert not errors, "\n".join(str(error) for error in errors)
    operations = 0
    for path in document["paths"].values():
        for operation in path.values():
            operations += 1
            for parameter in operation.get("parameters", []):
                validate_schema(parameter["schema"])
            for media in operation.get("requestBody", {}).get("content", {}).values():
                validate_schema(media["schema"])
            for response in operation["responses"].values():
                for media in response.get("content", {}).values():
                    validate_schema(media["schema"])
    if args.served:
        served = json.loads(args.served.read_text(encoding="utf-8"))
        assert served == document, "served and exported OpenAPI documents differ"
    if args.schemas:
        validate_boundaries(args.schemas)
    print(f"OpenAPI 3.1 GO: {operations} operations; schemas valid; offline equality checked={bool(args.served)}")


if __name__ == "__main__":
    main()
