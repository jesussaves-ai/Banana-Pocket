#!/usr/bin/env python3
# The bundled themes against docs/theme.schema.json, on the host (no device, no build).
#
#   tools/check-themes.py [repo root]     a line per theme, then any problems; nonzero if there were any
#   tools/check-themes.py --selftest      the validator on two inline documents (one good, one bad)
#
# The themes are read by the menu's own parser, which allows // and /* */ comments and trailing commas; this strips
# both (outside strings) so the file can go through json. The schema's keywords are implemented here, not ignored: a
# keyword this file does not know is reported, so the check cannot silently pass a constraint it never looked at.
import fnmatch, json, os, re, sys

ANNOTATIONS = {"$schema", "$id", "$comment", "title", "description", "default", "examples", "deprecated", "readOnly", "writeOnly"}
HANDLED = {"$ref", "$defs", "type", "enum", "pattern", "properties", "additionalProperties", "items", "anyOf", "oneOf", "required"}


def strip_comments(text):
    out, i, n = [], 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1]); i += 2; continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True; out.append(c); i += 1; continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            if j < 0: break
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0: break
            i = j + 2
            continue
        out.append(c); i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


def resolve(ref, root):
    if not ref.startswith("#/"):
        raise ValueError("only local $refs are supported: " + ref)
    node = root
    for part in ref[2:].split("/"):
        node = node[part.replace("~1", "/").replace("~0", "~")]
    return node


def is_type(value, want):
    return {
        "object": isinstance(value, dict),
        "array": isinstance(value, list),
        "string": isinstance(value, str),
        "boolean": isinstance(value, bool),
        "number": isinstance(value, (int, float)) and not isinstance(value, bool),
        "integer": isinstance(value, int) and not isinstance(value, bool),
        "null": value is None,
    }.get(want)


def validate(value, schema, root, path, errors):
    if schema is True:
        return
    if schema is False:
        errors.append(path + ": not allowed")
        return
    if "$ref" in schema:
        validate(value, resolve(schema["$ref"], root), root, path, errors)
    for key in schema:
        if key not in HANDLED and key not in ANNOTATIONS:
            errors.append("%s: schema keyword %r is not implemented by this checker" % (path, key))
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(is_type(value, t) for t in types):
            errors.append("%s: expected %s, got %s" % (path, "/".join(types), type(value).__name__))
            return
    if "enum" in schema and value not in schema["enum"]:
        errors.append("%s: %r is not one of %s" % (path, value, schema["enum"]))
    if "pattern" in schema and isinstance(value, str) and not re.search(schema["pattern"], value):
        errors.append("%s: %r does not match %s" % (path, value, schema["pattern"]))
    if "anyOf" in schema and not any(not errs for errs in [_try(value, s, root, path) for s in schema["anyOf"]]):
        errors.append("%s: matches none of the allowed shapes" % path)
    if "oneOf" in schema:
        hits = [s for s in schema["oneOf"] if not _try(value, s, root, path)]
        if len(hits) != 1:
            errors.append("%s: matches %d of the oneOf shapes (must be 1)" % (path, len(hits)))
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for req in schema.get("required", []):
            if req not in value:
                errors.append("%s: missing required %r" % (path, req))
        extra = schema.get("additionalProperties", True)
        for k, v in value.items():
            if k in props:
                validate(v, props[k], root, path + "." + k, errors)
            elif extra is False:
                errors.append("%s.%s: not allowed" % (path, k))
            elif isinstance(extra, dict):
                validate(v, extra, root, path + "." + k, errors)
    if isinstance(value, list) and isinstance(schema.get("items"), (dict, bool)):
        for i, v in enumerate(value):
            validate(v, schema["items"], root, "%s[%d]" % (path, i), errors)


def _try(value, schema, root, path):
    errs = []
    validate(value, schema, root, path, errs)
    return errs


def check(path, root):
    errors = []
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.loads(strip_comments(f.read()))
    except (OSError, ValueError) as e:
        return ["%s: %s" % (os.path.basename(path), e)]
    validate(doc, root, root, "", errors)
    return errors


def selftest():
    schema = {"$defs": {"colour": {"anyOf": [{"type": "string", "pattern": "^#"}, {"type": "boolean"}]}},
              "type": "object",
              "properties": {"base": {"enum": ["dark", "light"]}, "accent": {"$ref": "#/$defs/colour"}},
              "additionalProperties": True}
    good = {"base": "dark", "accent": "#ffcc00", "anything": 1}
    bad = {"base": "blue", "accent": 7}
    ok1 = not _try(good, schema, schema, "")
    ok2 = len(_try(bad, schema, schema, "")) == 2
    ok3 = bool(_try({"x": 1}, {"type": "object", "properties": {}, "additionalProperties": False}, {}, ""))
    print("check-themes selftest: %s" % ("ok" if ok1 and ok2 and ok3 else "fail"))
    return 0 if ok1 and ok2 and ok3 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    root_dir = next((a for a in argv[1:] if not a.startswith("-")), os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    schema_path = os.path.join(root_dir, "docs", "theme.schema.json")
    themes_dir = os.path.join(root_dir, "system", "rootfs", "usr", "share", "banana", "themes")
    try:
        with open(schema_path, encoding="utf-8") as f:
            schema = json.load(f)
    except (OSError, ValueError) as e:
        print("check-themes: cannot read %s: %s" % (schema_path, e))
        return 2
    files = sorted(fn for fn in os.listdir(themes_dir) if fnmatch.fnmatch(fn, "*.json")) if os.path.isdir(themes_dir) else []
    if not files:
        print("check-themes: no themes in %s" % themes_dir)
        return 2
    failed = 0
    for name in files:
        errors = check(os.path.join(themes_dir, name), schema)
        if errors:
            failed += 1
            print("FAIL %s" % name)
            for e in errors:
                print("      " + e)
        else:
            print("  ok: %s" % name)
    print("check-themes: %d theme(s), %d with problems" % (len(files), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
