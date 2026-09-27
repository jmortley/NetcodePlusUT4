Synthetic format-contract fixtures, not runtime captures.

These actor lines were formatted using the literal Identity, Timing and Attribution printf templates in NCFireDiagnostics.cpp (schema 2). Only dispatch/outcome rows carry scope; RECEIVE and client rows have generation=0. ACCEPT uses the fixed request on a stock firing state. Tests exercise the stored text without regenerating it. A live paired capture remains required.
