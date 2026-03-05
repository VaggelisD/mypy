# sqlglot-mypy

A fork of [mypy](https://github.com/python/mypy) (v1.19.1) with mypyc bug fixes needed for compiling [SQLGlot](https://github.com/tobymao/sqlglot).

[![PyPI](https://img.shields.io/pypi/v/sqlglot-mypy?color=blue)](https://pypi.org/project/sqlglot-mypy/)

## What is this?

SQLGlot uses [mypyc](https://mypyc.readthedocs.io/) to compile its core Python modules into C extensions for better performance. Upstream mypy/mypyc has several bugs that prevent correct compilation of SQLGlot. This fork maintains those fixes on top of the latest upstream release.

This package is **only used as a build tool** — it provides the `mypyc` compiler that generates C extensions for SQLGlot. It is not used at runtime. SQLGlot users who install pre-built wheels never interact with this package.

## Fixes included (on top of mypy 1.19.1)

- Fix vtable construction for deep trait inheritance
- Fix `init_subclass` running before `ClassVar` instantiation
- Fix shadow vtable misalignment for property getters/setters
- Fix `allow_interpreted_subclasses` not seeing subclass attribute overrides
- Fix cross-module class attribute defaults causing `KeyError`
- Fix comprehension with nested lambdas and move nested class to module level for mypyc compatibility
- Add `str.isspace()`, `str.isalnum()`, `str.isdigit()`, `str.lower()`, `str.upper()` primitives
- Optimize dataflow analysis: do not preemptively diff & union op-level sets

## Installation

```bash
pip install sqlglot-mypy
```

> **Note:** `sqlglot-mypy` and `mypy` both provide the `mypy` and `mypyc` packages. They **cannot coexist** in the same environment. Use `sqlglot-mypy` in a dedicated build environment (e.g., cibuildwheel's `before-build`) and keep upstream `mypy` for type checking / linting.

## Usage

This package is typically used indirectly through SQLGlot's build system:

```bash
# In the sqlglot repo — build the C extension locally
make install-devc
```

Or in CI via cibuildwheel:

```toml
# cibuildwheel config
[tool.cibuildwheel]
before-build = "pip install sqlglot-mypy"
```

## Versioning

This fork uses [PEP 440 post-releases](https://peps.python.org/pep-0440/#post-releases) to maintain parity with upstream mypy:

| Upstream mypy | sqlglot-mypy         |
|---------------|----------------------|
| 1.19.1        | 1.19.1.post1, .post2 |
| 1.19.2        | 1.19.2.post1, ...    |

The base version always matches upstream. The `.postN` suffix increments with each new fix release on our side.

## Releasing a new version

1. Make changes on the `release-1.19` branch
2. Run pre-push checks:
   ```bash
   cd /path/to/mypy

   # Type check
   python runtests.py self

   # IR build tests
   python -m pytest mypyc/test/test_irbuild.py -q

   # Runtime tests
   python -m pytest mypyc/test/test_run.py -q
   ```
3. Bump the version in `mypy/version.py` (e.g., `1.19.1.post2` → `1.19.1.post3`)
4. Commit, tag, and push:
   ```bash
   git commit -am "Bump version to 1.19.1.postN"
   git tag v1.19.1.postN
   git push fork release-1.19 --tags
   ```
5. The `build_wheels.yml` workflow will automatically build and publish to PyPI when the tag is pushed.

## Syncing with upstream

When upstream releases a new version (e.g., 1.19.2):

1. Fetch upstream tags: `git fetch origin --tags`
2. Create a new release branch from the upstream tag: `git checkout -b release-1.19 v1.19.2`
3. Cherry-pick or rebase fixes on top
4. Set version to `1.19.2.post1` and release as above

## CI/CD

This fork ships **pure Python wheels only** — mypyc is a build tool and doesn't need to be compiled itself. This keeps CI fast (1 build job vs 10+ for compiled wheels).

### Workflows

- **`build_wheels.yml`** — Triggered on `v*` tags. Builds an sdist and a pure Python wheel (`py3-none-any`), then publishes to PyPI via trusted publishing.
- **`test.yml`** — Triggered on pushes to `release*` branches and PRs. Runs:
  - Full mypyc test suite (`mypyc/test/`) on Python 3.9–3.14 (6 jobs)
  - Type check own code (`tox -e type`) on Python 3.9
  - Lint and formatting (`tox -e lint`) on Python 3.10

Upstream workflows not relevant to sqlglot-mypy (docs, mypy_primer, sync_typeshed, test_stubgenc) have been removed.

## Repository structure

- **Upstream remote** (`origin`): `https://github.com/python/mypy.git`
- **Fork remote** (`fork`): `https://github.com/VaggelisD/sqlglot-mypy.git`
- **Release branch**: `release-1.19`
- **PyPI**: [sqlglot-mypy](https://pypi.org/project/sqlglot-mypy/)

## License

This project is licensed under the MIT License, same as upstream mypy.
