"""
cfuture.lint — AST-based checker for closure captures in cfuture callbacks.

CFU001: callback captures '{name}' from outer scope — pass via deps=[{name}] instead.
CFU002: xi-protocol class '{name}' defined inside a function — move to module level
        so the worker interpreter can resolve it via import.

Ships as:
  - standalone checker: python -m cfuture.lint <file>
  - flake8 plugin via entry point: cfuture.lint:CfutureLintPlugin
"""

import ast
import builtins
import sys
from typing import List, Tuple, Generator


CHECKED_METHODS = {"then", "except_", "finally_", "submit"}
CFU001 = (
    "CFU001 callback captures '{name}' from outer scope"
    " — pass via deps=[{name}] instead"
)
CFU002 = (
    "CFU002 xi-protocol class '{name}' defined inside a function"
    " — move to module level so the worker can resolve it"
)

_BUILTINS: frozenset = frozenset(dir(builtins)) | {"True", "False", "None"}


def _collect_names_in_expr(node: ast.expr) -> List[str]:
    """Collect all Name nodes referenced in an expression."""
    return [n.id for n in ast.walk(node) if isinstance(n, ast.Name)]


def _get_lambda_params(node: ast.Lambda) -> set:
    args = node.args
    params = set()
    for a in args.args + args.posonlyargs + args.kwonlyargs:
        params.add(a.arg)
    if args.vararg:
        params.add(args.vararg.arg)
    if args.kwarg:
        params.add(args.kwarg.arg)
    return params


def _get_deps_names(call_node: ast.Call) -> set:
    """Extract names explicitly listed in deps=[...] kwarg."""
    deps_names = set()
    for kw in call_node.keywords:
        if kw.arg == "deps" and isinstance(kw.value, (ast.List, ast.Tuple)):
            for elt in kw.value.elts:
                if isinstance(elt, ast.Name):
                    deps_names.add(elt.id)
    return deps_names


def _has_xi_protocol(cls_node: ast.ClassDef) -> bool:
    """Return True if the class defines __xi_encode__ or is decorated with xi_dataclass."""
    for decorator in cls_node.decorator_list:
        name = None
        if isinstance(decorator, ast.Name):
            name = decorator.id
        elif isinstance(decorator, ast.Attribute):
            name = decorator.attr
        if name == "xi_dataclass":
            return True
    for node in ast.walk(cls_node):
        if isinstance(node, ast.FunctionDef) and node.name in (
            "__xi_encode__",
            "__xi_decode__",
        ):
            return True
    return False


def check_file(source: str, filename: str = "<unknown>") -> List[Tuple[int, int, str]]:
    """Return list of (line, col, message) tuples."""
    try:
        tree = ast.parse(source, filename=filename)
    except SyntaxError:
        return []

    errors = []

    # CFU001: closure captures in callbacks
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue

        method_name = None
        if isinstance(node.func, ast.Attribute):
            method_name = node.func.attr
        elif isinstance(node.func, ast.Name):
            method_name = node.func.id

        if method_name not in CHECKED_METHODS:
            continue

        if not node.args:
            continue
        cb_arg = node.args[0]

        if not isinstance(cb_arg, ast.Lambda):
            continue

        lambda_params = _get_lambda_params(cb_arg)
        deps_names = _get_deps_names(node)
        body_names = set(_collect_names_in_expr(cb_arg.body))
        free_names = body_names - lambda_params - deps_names - _BUILTINS

        for name in sorted(free_names):
            errors.append((cb_arg.lineno, cb_arg.col_offset, CFU001.format(name=name)))

    # CFU002: xi-protocol classes defined inside functions
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for child in ast.walk(node):
            if child is node:
                continue
            if isinstance(child, ast.ClassDef) and _has_xi_protocol(child):
                errors.append((
                    child.lineno,
                    child.col_offset,
                    CFU002.format(name=child.name),
                ))

    errors.sort()
    return errors


class CfutureLintPlugin:
    """flake8 plugin entry point."""

    name = "cfuture-lint"
    version = "0.1.0"
    off_by_default = False

    def __init__(self, tree: ast.AST):
        self._tree = tree

    def run(self) -> Generator:
        source = ast.unparse(self._tree)
        for lineno, col, msg in check_file(source):
            yield (lineno, col, msg, type(self))


def main():
    import argparse
    parser = argparse.ArgumentParser(description="cfuture lint checker")
    parser.add_argument("files", nargs="+", help="Python files to check")
    args = parser.parse_args()

    total_errors = 0
    for path in args.files:
        try:
            with open(path) as f:
                source = f.read()
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            continue
        errors = check_file(source, filename=path)
        for lineno, col, msg in errors:
            print(f"{path}:{lineno}:{col}: {msg}")
            total_errors += 1

    sys.exit(1 if total_errors else 0)


if __name__ == "__main__":
    main()
