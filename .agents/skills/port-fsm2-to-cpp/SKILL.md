---
name: port-fsm2-to-cpp
description: Focuses on the translation of FSM2 from Fortran to C++ and identifies possible mismatches during the porting process.
---

# Port FSM2 to C++

Use this skill when the user asks to:
- Translate FSM2 (Flexible Snow Model 2) Fortran modules and subroutines into Unreal Engine C++ classes/structs.
- Identify bugs, numerical instability, or feature mismatches between the original Fortran FSM2 implementation and the C++ port.
- Compare and validate FSM2 outputs (temperature, density, SWE, run-off, etc.) in the ported C++ version against Fortran baseline outputs.

## Principle
When porting scientific code (such as FSM2) from Fortran to C++, a 1:1 functional translation must be achieved before any C++/Unreal optimization is applied. Array indexing (1-based vs 0-based), memory layout (column-major vs row-major), and strict type conversion (floating-point precision) must be carefully audited to prevent silent errors or deviations in the simulation.

## Targets
Primary domains to apply this skill:
- Reviewing Fortran `.F90` source files against their corresponding `.cpp` / `.h` counterparts.
- Identifying and fixing floating-point discrepancies or off-by-one errors in snow layer iterations.

## Guidelines

### A. Array Indexing and Bounds
Fortran natively uses 1-based indexing, whereas C++ uses 0-based.
- **Audit Off-by-One Errors**: Thoroughly inspect loops (`DO` vs `for`) handling snow layers. Ensure boundary conditions (e.g., surface layer and ground layer) are accessed correctly.
- **Direction of Iteration**: Pay attention to the physical interpretation of indices (e.g., does index 0 represent the top layer or bottom layer in the C++ arrays compared to Fortran 1?).

### B. Memory Layout & Multi-dimensional Arrays
Fortran is column-major; C++ is row-major.
- **Flattened Arrays**: If 2D arrays are flattened into 1D arrays for performance, ensure the indexing arithmetic matches the expected traversal order.
- **Vectorization / TArrays**: When using Unreal Engine's `TArray`, ensure proper allocation/resizing matching the Fortran allocation logic.

### C. Precision and Data Types
Fortran's `REAL(KIND=8)` or `DOUBLE PRECISION` maps to `double` in C++.
- **Maintain Precision**: Be wary of implicitly casting `double` to `float`, specifically during intermediate calculations involving accumulation, exponentiation, or division.
- **Literal Constants**: Verify floating point literal representations (e.g., `1.0_8` in Fortran to `1.0` or `1.0f` in C++). Beware of `float` vs `double` precision loss.

### D. Control Flow and Subroutine Parameter Passing
Fortran subroutines pass by reference by default, and use `INTENT(IN/OUT)`.
- **In-Out Parameters**: In C++, use pointers (`*`) or references (`&`) for variables that act as `INTENT(INOUT)` or `INTENT(OUT)` arguments in the Fortran subroutines. Use `const &` for `INTENT(IN)`.
- **Early Exits/Bailouts**: Double-check conditions that cause subroutines to return early or skip computation for certain layers.
- **Global States and Modules**: Watch out for Fortran module variables that act as global state. These should ideally be encapsulated in C++ class instances to avoid side effects in parallel processing.

### E. Mismatch Identification Strategy
- **Unit Testing**: Compare outputs for single cells under identical forcing data. Keep track of the initial states.
- **Checkpoint Logging**: Temporarily enable verbose logging at identical check-points in both the Fortran code output and the C++ output steps to trace divergences layer by layer, time-step by time-step.
- **Threshold Divergence**: Identify the exact time-step where variables like Density or Temperature diverge by more than a tiny epsilon. Identify the mathematical operation causing the divergence.
