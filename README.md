## BishengJDK 11 RISC-V port

BishengJDK 11 now brings the template interpreter and backends of C1/C2 compiler to the RISC-V world. We supports RV64G (G used to be represent the IMAFD base and extensions of RISC-V ISA) with BV (bit-manipulation and vector extensions) on the way, and the compressed instructions are out of plan. 

## How to configure and build the JDK on RISCV64?

Please refer to [the building suggestions](./BUILDING.md).

## License

BishengJDK follows GPLv2 with Classpath Exception，see [License](https://gitee.com/openeuler/bishengjdk-11/blob/master/LICENSE)。

## State of Project

- Features:
  - We almost cover all components of jdk11, except AOT/Graal and Shenandoah GC. The Shenandoah GC will comming soon in the near future, and AOT and Graal is depending. Note that the OpenJDK community has disabled the features in Oracle builds in [JDK16](https://github.com/openjdk/jdk/pull/960).

  - B/V extensions
    - The both two extensions of ISA are currently draft versions and not released yet. V is close to stable, and so we will introduces the vector instructions to the C2 backends and intrinsics soon.

  - Intrinsics
    - Lots of intrinsics depends on the B extension and will be dedicate if wrote by the B/V instructions, and other intrinsics of few methods of String and Arrays are now available.

- Considerations:
  - Frame layout
    - Java frames are organized as other platforms, following the fp links, but native frames are arranged as the riscv style, of which the fp pointing to the sender sp. 
 
  - Cross-modifying code
    - We believe that the same story happened on the riscv machines. Details of the issue see https://bugs.openjdk.java.net/browse/JDK-8223613. We followed the conservative solution as same as the Aarch64 port, deoptimization when patching in C1. And the rest of risky may be addressed by [the invoke bindings JEP](https://openjdk.java.net/jeps/8221828).

  - Comparing to other architecutures, more efforts were paid in the code genaration for addressing the short offset of branch instructions, the no-flag-register architecture, the simplest addressing mode and so on.

- Tests
  - We have passed 17000+ jtreg test cases, cross-compiled on x86 machines and ran on the user mode of QEMU RISCV64. And we hope to promote more tests and benchmarks on the real riscv machine in the future if possible.

## By the way, Who is Bisheng?

[Bi Sheng (990–1051 AD) was the Chinese inventor of the first known movable type technology. Bi Sheng's system was made of Chinese porcelain and was invented between 1041 and 1048 during the Song dynasty](http://www.edubilla.com/inventor/bi-sheng/).
