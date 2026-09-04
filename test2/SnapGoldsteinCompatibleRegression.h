#pragma once

// Fixture-driven SNAP numerical regression. The external H5 must contain
// fixture_contract=snap_goldstein_compatible_fixture_v1 and four case_* groups.
// Run with: test2.exe --snap-goldstein-compatible-regression <fixture.h5>
int RunSnapGoldsteinCompatibleRegression(const char* fixturePath);
