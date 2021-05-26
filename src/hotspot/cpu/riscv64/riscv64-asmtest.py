#!/usr/bin/env python
# -*- coding: utf-8 -*-

import random

RISCV64_AS = "<PATH-TO-AS>"
RISCV64_OBJDUMP = "<PATH-TO-OBJDUMP>"
RISCV64_OBJCOPY = "<PATH-TO-OBJCOPY>"

class Operand(object):

     def generate(self):
        return self

class Register(Operand):

    def generate(self):
        self.number = random.randint(0, 31)
        return self

    def astr(self, prefix):
        return prefix + str(self.number)

class VectorRegister(Register):

    def generate(self):
        self.number = random.randint(0, 31)
        return self

    def __str__(self):
        return self.astr("v")

class FloatRegister(Register):

    def __str__(self):
        return self.astr("f")

    def nextReg(self):
        next = FloatRegister()
        next.number = (self.number + 1) % 32
        return next

class GeneralRegister(Register):

    def generate(self):
        self.number = random.randint(5, 31)
        return self

    def __str__(self):
        return self.astr("x")

class GeneralRegisterOrZr(Register):

    def generate(self):
        self.number = random.randint(0, 31)
        return self

    def astr(self, prefix = ""):
        if (self.number == 0):
            return "zr"
        else:
            return prefix + str(self.number)

    def __str__(self):
        if (self.number == 0):
            return self.astr()
        else:
            return self.astr("x")

class GeneralRegisterOrSp(Register):
    def generate(self):
        self.number = random.randint(1, 31)
        return self

    def astr(self, prefix = ""):
        if (self.number == 2):
            return "sp"
        else:
            return prefix + str(self.number)

    def __str__(self):
        if (self.number == 2):
            return self.astr()
        else:
            return self.astr("x")

class OperandFactory:

    _modes = {'x' : GeneralRegister,
              'z' : GeneralRegisterOrZr,
              'f' : FloatRegister,
              'v' : VectorRegister}

    @classmethod
    def create(cls, mode):
        return OperandFactory._modes[mode]()

class Instruction(object):

    def __init__(self, name):
        self._name = name

    def aname(self):
        return self._name.replace('_', '.')

    def emit(self) :
        pass

    def compare(self) :
        pass

    def generate(self) :
        return self

    def cstr(self):
        return '__ %s(' % self.name()

    def astr(self):
        return '%s  ' % self.aname()

    def name(self):
        name = self._name
        if name == "and":
            name = "andr"
        elif name == "or":
            name = "orr"
        elif name == "not":
            name = "notr"
        elif name == "xor":
            name = "xorr"
        return name

    def multipleForms(self):
         return 0

class ThreeRegInstruction(Instruction):

    def generate(self):
        self.reg = [GeneralRegister().generate(), GeneralRegister().generate(),
                    GeneralRegister().generate()]
        return self


    def cstr(self):
        return (super(ThreeRegInstruction, self).cstr()
                + ('%s, %s, %s'
                   % (self.reg[0],
                      self.reg[1], self.reg[2])))

    def astr(self):
        return (super(ThreeRegInstruction, self).astr()
                + ('%s, %s, %s'
                   % (self.reg[0],
                      self.reg[1], self.reg[2])))

class TwoRegInstruction(Instruction):

    def generate(self):
        self.reg = [GeneralRegister().generate(), GeneralRegister().generate()]
        return self

    def cstr(self):
        return (super(TwoRegInstruction, self).cstr()
                + '%s, %s' % (self.reg[0], self.reg[1]))

    def astr(self):
        return (super(TwoRegInstruction, self).astr()
                + '%s, %s' % (self.reg[0], self.reg[1]))

class TwoRegImmedOp(TwoRegInstruction):

    def generate(self):
        super(TwoRegImmedOp, self).generate()
        self.immed = random.randint(-1<<11, (1<<11)-1)
        return self

    def cstr(self):
        return (super(TwoRegImmedOp, self).cstr()
                + ', %s);' % self.immed)

    def astr(self):
        return (super(TwoRegImmedOp, self).astr()
                + ', %s' % self.immed)

class TwoRegUnsignedImmedOp(TwoRegImmedOp):

    def generate(self):
        super(TwoRegUnsignedImmedOp, self).generate()
        self.immed = random.randint(0, 1<<12 -1)
        return self

    def cstr(self):
        return (super(TwoRegImmedOp, self).cstr()
                + ', %su);' % self.immed)

class LoadImmedOp(Instruction):

    def generate(self):
        super(LoadImmedOp, self).generate()
        self.immed = hex(random.randint(0, 1<<20 -1))
        self.reg = GeneralRegister().generate()
        return self

    def cstr(self):
        return (super(LoadImmedOp, self).cstr()
                + '%s, %s000);' % (self.reg, self.immed))

    def astr(self):
        return (super(LoadImmedOp, self).astr()
                + '%s, %s' % (self.reg, self.immed))

class OneRegOp(Instruction):

    def generate(self):
        self.reg = GeneralRegister().generate()
        return self

    def cstr(self):
        return (super(OneRegOp, self).cstr()
                + '%s);' % self.reg)

    def astr(self):
        return (super(OneRegOp, self).astr()
                + '%s' % self.reg)

class ArithOp(ThreeRegInstruction):

    def generate(self):
        super(ArithOp, self).generate()
        return self

    def cstr(self):
        return ('%s);'
                % ThreeRegInstruction.cstr(self))

    def astr(self):
        return ThreeRegInstruction.astr(self)

class MultiOp():

    def multipleForms(self):
         return 3

    def forms(self):
         return ["__ pc()", "back", "forth"]

    def aforms(self):
         return [".", "back", "forth"]

class AbsOp(MultiOp, Instruction):

    def cstr(self):
        return super(AbsOp, self).cstr() + "%s);"

    def astr(self):
        return Instruction.astr(self) + "%s"

class RegAndAbsOp(MultiOp, Instruction):

    def generate(self):
        Instruction.generate(self)
        self.reg = GeneralRegister().generate()
        return self

    def cstr(self):
        return (super(RegAndAbsOp, self).cstr()
                + "%s, %s);" % (self.reg, "%s"))

    def astr(self):
        return "%s  %s, %s" % (self._name, self.reg, "%s")

class TwoRegAndAbsOp(MultiOp, TwoRegInstruction):

    def generate(self):
        super(TwoRegAndAbsOp, self).generate()
        return self

    def cstr(self):
        return (super(TwoRegAndAbsOp, self).cstr()
                + ', %s);' % "%s")

    def astr(self):
        return (super(TwoRegAndAbsOp, self).astr()
                + ', %s' % "%s")

class ShiftRegOp(ThreeRegInstruction):

    def generate(self):
        super(ShiftRegOp, self).generate()
        return self

    def cstr(self):
        return ('%s);'
                % ThreeRegInstruction.cstr(self))

    def astr(self):
        return ThreeRegInstruction.astr(self)

class ShiftImmOp(TwoRegUnsignedImmedOp):

    def generate(self):
        super(ShiftImmOp, self).generate()
        self.immed = random.randint(0, 1<<5 -1)
        return self

class Op(Instruction):

    def cstr(self):
        return Instruction.cstr(self) + ");"

    def astr(self):
        return '%s' % self.aname()

class SystemOp(Instruction):

     def __init__(self, op):
          Instruction.__init__(self, op[0])
          self.barriers = op[1]

     def generate(self):
          Instruction.generate(self)
          self.barrier \
              = [self.barriers[random.randint(0, len(self.barriers)-1)],
                 self.barriers[random.randint(0, len(self.barriers)-1)]]
          return self

     def cstr(self):
          return super(SystemOp, self).cstr() + "%su, %su);" \
                 % (self.barrier[0][0], self.barrier[1][0])

     def astr(self):
          return super(SystemOp, self).astr() + "%s, %s" % (self.barrier[0][1], self.barrier[1][1])

class CsrxixOp(Instruction):

    def generate(self):
        super(CsrxixOp, self).generate()
        self.reg = [GeneralRegister().generate(), GeneralRegister().generate()]
        self.immed = random.randint(0, (1<<12) - 1)
        return self

    def cstr(self):
        return super(CsrxixOp, self).cstr() + "%s, %su, %s);" % (self.reg[0], self.immed, self.reg[1])

    def astr(self):
        return super(CsrxixOp, self).astr() + "%s, %s, %s" % (self.reg[0], hex(self.immed), self.reg[1])

class CsrxiiOp(Instruction):

    def generate(self):
        super(CsrxiiOp, self).generate()
        self.reg = GeneralRegister().generate()
        self.immed = [random.randint(0, (1<<12) - 1), random.randint(0, (1<<5) - 1)]
        return self

    def cstr(self):
        return super(CsrxiiOp, self).cstr() + "%s, %su, %su);" % (self.reg, self.immed[0], self.immed[1])

    def astr(self):
        return super(CsrxiiOp, self).astr() + "%s, %s, %s" % (self.reg, hex(self.immed[0]), self.immed[1])

class CsrxiOp(Instruction):

    def generate(self):
        super(CsrxiOp, self).generate()
        self.reg = GeneralRegister().generate()
        self.immed = hex(random.randint(0, (1<<12) - 1))
        return self

    def cstr(self):
        return super(CsrxiOp, self).cstr() + "%s, %s);" % (self.reg, self.immed)

    def astr(self):
        return super(CsrxiOp, self).astr() + "%s, %s" % (self.reg, self.immed)

class CsriiOp(Instruction):

    def generate(self):
        super(CsriiOp, self).generate()
        self.immed = [hex(random.randint(0, (1<<12) - 1)), random.randint(0, (1<<5) - 1)]
        return self

    def cstr(self):
        return super(CsriiOp, self).cstr() + "%su, %s);" % (self.immed[0], self.immed[1])

    def astr(self):
        return super(CsriiOp, self).astr() + "%s, %s" % (self.immed[0], self.immed[1])

class CsrixOp(Instruction):

    def generate(self):
        super(CsrixOp, self).generate()
        self.reg = GeneralRegister().generate()
        self.immed = hex(random.randint(0, (1<<12) - 1))
        return self

    def cstr(self):
        return super(CsrixOp, self).cstr() + "%s, %s);" % (self.immed, self.reg)

    def astr(self):
        return super(CsrixOp, self).astr() + "%s, %s" % (self.immed, self.reg)

class AtomOp(Instruction):

    def __init__(self, args):
        self._name, self.size, self.suffix = args

    def generate(self):
        self.asmname = "%s.%s.%s" % (self._name, self.size, self.suffix)
        self._name = "%s_%s" % (self._name, self.size)
        self.reg = [GeneralRegister().generate(), GeneralRegister().generate(),
                    GeneralRegister().generate()]
        return self

    def aname(self):
        return self.asmname

    def cstr(self):
        if self._name.startswith("lr"):
            return super(AtomOp, self).cstr() + "%s, %s, Assembler::%s);" \
                   % (self.reg[0], self.reg[1], self.suffix)
        return super(AtomOp, self).cstr() + "%s, %s, %s, Assembler::%s);" \
                   % (self.reg[0], self.reg[1], self.reg[2], self.suffix)

    def astr(self):
        if self._name.startswith("lr"):
            return super(AtomOp, self).astr() + "%s, (%s)" \
                   % (self.reg[0], self.reg[1])
        elif self._name.startswith("sc"):
            return super(AtomOp, self).astr() + "%s, %s, (%s)" \
               % (self.reg[0], self.reg[1], self.reg[2])
        return super(AtomOp, self).astr() + "%s, %s, (%s)" \
               % (self.reg[0], self.reg[2], self.reg[1])

class TwoRegOp(TwoRegInstruction):

    def cstr(self):
        return TwoRegInstruction.cstr(self) + ");"

class ThreeRegOp(ThreeRegInstruction):

    def cstr(self):
        return ThreeRegInstruction.cstr(self) + ");"

class Address(object):

    def generate(self):
        self.base = GeneralRegister().generate()
        self.offset = random.randint(-1<<11, 1<<11-1)
        return self

    def cstr(self):
        result = "Address(%s, %s)" % (self.base.astr("x"), self.offset)
        return result

    def astr(self):
        result = "%s(%s)" % (self.offset, self.base.astr("x"))
        return result

class LoadStoreOp(Instruction):

    def __init__(self, name):
        Instruction.__init__(self, name)

    def generate(self):
        self.adr = Address().generate()
        if (self._name.startswith("f")):
            self.reg = FloatRegister().generate()
        else:
            self.reg = GeneralRegister().generate()
        return self

    def cstr(self):
        return "%s%s, %s);" % (Instruction.cstr(self), self.reg, self.adr.cstr())

    def astr(self):
        return "%s%s, %s" % (Instruction.astr(self), self.reg, self.adr.astr())

class FloatInstruction(Instruction):

    def __init__(self, args):
        if (len(args) == 3):
            name, self.modes, self.roundingModes = args
        else:
            name, self.modes = args
            self.roundingModes = ""
        Instruction.__init__(self, name)

    def generate(self):
        self.reg = [OperandFactory.create(self.modes[i]).generate()
                    for i in range(self.numRegs)]
        return self

    def cstr(self):
        if (self.roundingModes == ""):
            formatStr = "%s%s" + ''.join([", %s" for i in range(1, self.numRegs)] + [");"])
        else:
            formatStr = "%s%s" + ''.join([", %s" for i in range(1, self.numRegs)] + [", Assembler::" + self.roundingModes + ");"])
        return (formatStr
                % tuple([Instruction.cstr(self)] +
                        [str(self.reg[i]) for i in range(self.numRegs)])) # Yowza

    def astr(self):
        name = self._name
        if (self.roundingModes == "") | (name == "fcvt_d_w") | (name == "fcvt_d_wu") | (name == "fcvt_d_s"):
            formatStr = "%s%s" + ''.join([", %s" for i in range(1, self.numRegs)])
        else:
            formatStr = "%s%s" + ''.join([", %s" for i in range(1, self.numRegs)]) + ", " + self.roundingModes
        return (formatStr
                % tuple([Instruction.astr(self)] +
                        [(self.reg[i].astr(self.modes[i])) for i in range(self.numRegs)]))

class TwoRegFloatOp(FloatInstruction):
    numRegs = 2

class ThreeRegFloatOp(TwoRegFloatOp):
    numRegs = 3

class Float2ArithOp(FloatInstruction):
    numRegs = 2

class Float3ArithOp(TwoRegFloatOp):
    numRegs = 3

class Float4ArithOp(TwoRegFloatOp):
    numRegs = 4

class FloatConvertOp(TwoRegFloatOp):
    numRegs = 2

class VectorInstruction(Instruction):

    def __init__(self, args):
        name, self.modes, self.mask = args
        Instruction.__init__(self, name)

    def generate(self):
        self.numRegs = len(self.modes)
        self.reg = [OperandFactory.create(self.modes[i]).generate()
                    for i in range(self.numRegs)]
        return self

class AMOOp(VectorInstruction):

    def generate(self):
        super(AMOOp, self).generate()
        return self

    def cstr(self):
        forma = ", %s, %s" % (self.reg[1], self.reg[2])
        if (self.modes[0] == 'z'):
            forma+=", false"
        else:
            forma+=", true"
        pre = "%s%s" % (VectorInstruction.cstr(self), self.reg[3])
        if (self.mask == 1):
            return pre+forma+");"
        else:
            return pre+forma+", Assembler::v0_t);"

    def astr(self):
        forma = ", (%s), %s, %s" % (self.reg[1], self.reg[2], self.reg[3])
        if (self.modes[0] == 'z'):
            pre = "%s%s" % (VectorInstruction.astr(self), "x0")
        else:
            pre = "%s%s" % (VectorInstruction.astr(self), self.reg[3])
        if (self.mask == 1):
            return pre+forma
        else:
            return pre+forma+", v0.t"

class R2vmOp(VectorInstruction):

    def generate(self):
        super(R2vmOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class R2rdOp(VectorInstruction):

    def generate(self):
        super(R2rdOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1])
        return forma+");"

    def astr(self):
        forma = "%s%s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1])
        return forma+""

class LdStTwoRegOp(VectorInstruction):

    def generate(self):
        super(LdStTwoRegOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, (%s)" % (Instruction.astr(self), self.reg[0], self.reg[1])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class LdStThreeoRegOp(VectorInstruction):

    def generate(self):
        super(LdStThreeoRegOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.reg[2])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, (%s), %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.reg[2])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class OneRegVOp(VectorInstruction):

    def generate(self):
        super(OneRegVOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s" % (Instruction.cstr(self), self.reg[0])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s" % (Instruction.astr(self), self.reg[0])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class ThreeVecRegD12Op(VectorInstruction):

    def generate(self):
        super(ThreeVecRegD12Op, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.reg[2])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.reg[2])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class ThreeVecRegD21Op(VectorInstruction):

    def generate(self):
        super(ThreeVecRegD21Op, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[2], self.reg[1])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[2], self.reg[1])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class ResSubVecRegOp(VectorInstruction):

    def generate(self):
        super(ResSubVecRegOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.reg[2])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[2], self.reg[1])
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class ThreeVecRegD21VOp(VectorInstruction):

    def generate(self):
        super(ThreeVecRegD21VOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[2], self.reg[1])
        return forma+", v0);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[2], self.reg[1])
        return forma+", v0"

class OneVecRegImmOp(VectorInstruction):

    def generate(self):
        super(OneVecRegImmOp, self).generate()
        self.immed = random.randint(-1<<4, (1<<4) - 1)
        return self

    def cstr(self):
        forma = "%s%s, %s" % (Instruction.cstr(self), self.reg[0], self.immed)
        return forma+");"

    def astr(self):
        forma = "%s%s, %s" % (Instruction.astr(self), self.reg[0], self.immed)
        return forma+""

class TwoVecRegImmOp(VectorInstruction):

    def generate(self):
        super(TwoVecRegImmOp, self).generate()
        self.immed = random.randint(-1<<4, (1<<4) - 1)
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.immed)
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.immed)
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class ImmSubOp(VectorInstruction):

    def generate(self):
        super(ImmSubOp, self).generate()
        self.immed = random.randint(-1<<4, (1<<4) - 1)
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.immed, self.reg[1])
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.immed)
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class TwoVecRegImmVOp(VectorInstruction):

    def generate(self):
        super(TwoVecRegImmVOp, self).generate()
        self.immed = random.randint(-1<<4, (1<<4) - 1)
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.immed)
        return forma+", v0);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.immed)
        return forma+", v0"

class TwoVecRegUnsignedImmOp(VectorInstruction):

    def generate(self):
        super(TwoVecRegUnsignedImmOp, self).generate()
        self.immed = random.randint(0, (1<<5) - 1)
        return self

    def cstr(self):
        forma = "%s%s, %s, %su" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.immed)
        if (self.mask == 1):
            return forma+");"
        else:
            return forma+", Assembler::v0_t);"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.immed)
        if (self.mask == 1):
            return forma+""
        else:
            return forma+", v0.t"

class CfgOp(VectorInstruction):

    def generate(self):
        super(CfgOp, self).generate()
        return self

    def cstr(self):
        forma = "%s%s, %s, %s" % (Instruction.cstr(self), self.reg[0], self.reg[1], self.reg[2])
        return forma+");"

    def astr(self):
        forma = "%s%s, %s, %s" % (Instruction.astr(self), self.reg[0], self.reg[1], self.reg[2])
        return forma+""

def generate_base(kind, names):
    for name in names:
        for i in range(1):
            op = kind(name).generate()
            if op.multipleForms():
                forms = op.forms()
                aforms = op.aforms()
                for i in range(op.multipleForms()):
                    cstr = op.cstr() % forms[i]
                    astr = op.astr() % aforms[i]
                    print "    %-50s // %-s" % (cstr, astr)
                    outfile.write("%-s\n" %(astr))
            else:
                print "    %-50s // %-s" %(op.cstr(), op.astr())
                outfile.write("%-s\n" %(op.astr()))

def print_op(kind):
    outfile.write("# " + kind.__name__ + "\n");
    print "\n    // " + kind.__name__

def generate(kind, names):
    print_op(kind)
    generate_base(kind, names)

with open("riscv64ops.s", "w") as outfile:

    print "// BEGIN  Generated code -- do not edit"
    print "// Generated by riscv64-asmtest.py"

    print "    Label back, forth;"
    print "    __ bind(back);"

    outfile.write("back:\n")

    generate (ArithOp,
              [ "add", "sub", "addw", "subw",
                "or", "xor", "mul", "mulh",
                "mulhsu", "mulhu", "div", "divu",
                "rem", "remu", "mulw", "divw",
                "divuw", "remw", "remuw", "and"])

    generate (TwoRegImmedOp,
              [ "addi", "addiw", "ori", "xori",
                "andi", "slti", "jalr"])

    generate (TwoRegUnsignedImmedOp, ["sltiu"])

    generate (AbsOp, [ "j", "jal" ])

    generate (LoadImmedOp, ["lui", "auipc"])

    generate (RegAndAbsOp, ["bnez", "beqz"])

    generate (TwoRegAndAbsOp, ["bne", "beq", "bge", "bgeu",
                               "blt", "bltu"])

    generate (ShiftRegOp, ["sll", "srl", "sra", "sraw",
                           "sllw", "srlw"])

    generate (ShiftImmOp, ["slli", "srli", "srai", "slliw",
                           "srliw", "sraiw"])

    generate (Op, ["nop", "ecall", "ebreak", "fence_i"])

    barriers = [["8", "i"], ["4", "o"], ["2", "r"], ["1", "w"],
                ["10", "ir"], ["5", "ow"], ["15", "iorw"]]

    generate (SystemOp, [["fence", barriers]])

    print_op (AtomOp)
    for size in ("w", "d"):
        for suffix in ("aq", "rl"):
            generate_base (AtomOp, [["sc", size, suffix], ["amoswap", size, suffix],
                               ["amoadd", size, suffix], ["amoxor", size, suffix],
                               ["amoand", size, suffix], ["amoor", size, suffix],
                               ["amomin", size, suffix], ["amomax", size, suffix],
                               ["amominu", size, suffix], ["amomaxu", size, suffix],
                               ["lr", size, suffix]])

    generate (OneRegOp, ["frflags", "frrm",
                         "frcsr", "rdtime", "rdcycle",
                         "rdinstret"])

    generate(TwoRegOp,
             ["mv", "not", "neg", "negw",
              "sext_w", "seqz", "snez", "sltz",
              "sgtz", "fscsr", "fsrm", "fsflags"])

    generate(ThreeRegOp,
             ["slt", "sltu"])

    generate(CsrxixOp, ["csrrw", "csrrs", "csrrc"])

    generate(CsrxiiOp, ["csrrwi", "csrrsi", "csrrci"])

    generate(CsrxiOp, ["csrr"])

    generate(CsrixOp, ["csrw", "csrs", "csrc"])

    generate(CsriiOp, ["csrwi", "csrsi", "csrci"])

    generate(LoadStoreOp, ["ld", "lw", "lwu", "lh",
                           "lhu", "lb", "lbu", "sd",
                           "sw", "sh", "sb", "fld",
                           "flw", "fsd", "fsw"])

    generate (Float2ArithOp, [["fsqrt_s", "ff", "rdn"], ["fsqrt_d", "ff", "rdn"]])

    generate (Float3ArithOp, [["fadd_s", "fff", "rup"], ["fsub_s", "fff", "rup"],
                              ["fadd_d", "fff", "rup"], ["fsub_d", "fff", "rup"],
                              ["fmul_s", "fff", "rup"], ["fdiv_s", "fff", "rup"],
                              ["fmul_d", "fff", "rup"], ["fdiv_d", "fff", "rup"]])

    generate (Float4ArithOp, [["fmadd_s", "ffff", "rup"], ["fmsub_s", "ffff", "rtz"],
                              ["fmadd_d", "ffff", "rup"], ["fmsub_d", "ffff", "rtz"],
                              ["fnmsub_s", "ffff", "rmm"], ["fnmadd_s", "ffff", "rtz"],
                              ["fnmsub_d", "ffff", "rmm"], ["fnmadd_d", "ffff", "rtz"]])

    generate(TwoRegFloatOp, [["fclass_s", "xf"], ["fmv_s", "ff"],
                             ["fclass_d", "xf"], ["fmv_d", "ff"],
                             ["fabs_s", "ff"], ["fneg_s", "ff"],
                             ["fabs_d", "ff"], ["fneg_d", "ff"],
                             ["fmv_x_w", "xf"], ["fmv_x_d", "xf"]])

    generate(ThreeRegFloatOp, [["fsgnj_s", "fff"], ["fsgnjn_s","fff"],
                               ["fsgnj_d", "fff"], ["fsgnjn_d","fff"],
                               ["fsgnjx_s", "fff"], ["fmin_s", "fff"],
                               ["fsgnjx_d", "fff"], ["fmin_d", "fff"],
                               ["fmax_s", "fff"], ["feq_s", "xff"],
                               ["fmax_d", "fff"], ["feq_d", "xff"],
                               ["flt_s", "xff"], ["fle_s", "xff"],
                               ["flt_d", "xff"], ["fle_d", "xff"]])

    generate(FloatConvertOp, [["fcvt_w_s", "xf", "rup"], ["fcvt_wu_s", "xf", "rne"],
                              ["fcvt_s_w", "fx", "rdn"], ["fcvt_s_wu", "fx", "rtz"],
                              ["fcvt_l_s", "xf", "rne"], ["fcvt_lu_s", "xf", "rmm"],
                              ["fcvt_s_l", "fx", "rup"], ["fcvt_s_lu", "fx", "rtz"],
                              ["fcvt_s_d", "ff", "rdn"], ["fcvt_d_s", "ff", "rne"],
                              ["fcvt_w_d", "xf", "rdn"], ["fcvt_wu_d", "xf", "rdn"],
                              ["fcvt_d_w", "fx", "rne"], ["fcvt_d_wu", "fx", "rne"],
                              ["fcvt_l_d", "xf", "rdn"], ["fcvt_lu_d", "xf", "rdn"],
                              ["fcvt_d_l", "fx", "rdn"], ["fcvt_d_lu", "fx", "rdn"]])

    print_op(AMOOp)
    for suffix in ("ei8_v", "ei16_v", "ei32_v"):
        for reg in ("vxvv", "zxvv"):
            for i in (0, 1):
                generate_base(AMOOp, [["vamoswap"+suffix, reg, i], ["vamoadd"+suffix, reg, i],
                                      ["vamoxor"+suffix, reg, i], ["vamoand"+suffix, reg, i],
                                      ["vamoor"+suffix, reg, i], ["vamomin"+suffix, reg, i],
                                      ["vamomax"+suffix, reg, i], ["vamominu"+suffix, reg, i],
                                      ["vamomaxu"+suffix, reg, i]])

    print_op(R2vmOp)
    for i in (0, 1):
        generate_base(R2vmOp, [["vzext_vf2", "vv", i], ["vzext_vf4", "vv", i],
                               ["vzext_vf8", "vv", i], ["vsext_vf2", "vv", i],
                               ["vsext_vf4", "vv", i], ["vsext_vf8", "vv", i],
                               ["vpopc_m", "xv", i], ["vfirst_m", "xv", i],
                               ["vmsbf_m", "vv", i], ["vmsif_m", "vv", i],
                               ["vmsof_m", "vv", i], ["viota_m", "vv", i],
                               ["vfcvt_xu_f_v", "vv", i], ["vfcvt_x_f_v", "vv", i],
                               ["vfcvt_f_xu_v", "vv", i], ["vfcvt_f_x_v", "vv", i],
                               ["vfcvt_rtz_xu_f_v", "vv", i], ["vfcvt_rtz_x_f_v", "vv", i],
                               ["vfwcvt_xu_f_v", "vv", i], ["vfwcvt_x_f_v", "vv", i],
                               ["vfwcvt_f_xu_v", "vv", i], ["vfwcvt_f_x_v", "vv", i],
                               ["vfwcvt_f_f_v", "vv", i], ["vfwcvt_rtz_xu_f_v", "vv", i],
                               ["vfwcvt_rtz_x_f_v", "vv", i], ["vfncvt_xu_f_w", "vv", i],
                               ["vfncvt_x_f_w", "vv", i], ["vfncvt_f_xu_w", "vv", i],
                               ["vfncvt_f_x_w", "vv", i], ["vfncvt_f_f_w", "vv", i],
                               ["vfncvt_rod_f_f_w", "vv", i], ["vfncvt_rtz_xu_f_w", "vv", i],
                               ["vfncvt_rtz_x_f_w", "vv", i], ["vfsqrt_v", "vv", i],
                               ["vfclass_v", "vv", i]])

    generate(R2rdOp, [["vmv1r_v", "vv", 1], ["vmv2r_v", "vv", 1],
                      ["vmv4r_v", "vv", 1], ["vmv8r_v", "vv", 1],
                      ["vfmv_f_s", "fv", 1], ["vmv_x_s", "xv", 1],
                      ["vfmv_s_f", "vf", 1], ["vmv_s_x", "vx", 1],
                      ["vfmv_v_f", "vf", 1], ["vmv_v_x", "vx", 1],
                      ["vmv_v_v", "vv", 1]])

    generate(OneVecRegImmOp, [["vmv_v_i", "v", 1]])

    generate(OneRegVOp, [["vid_v", "v", 1]])

    generate(LdStTwoRegOp, [["vl1r_v", "vx", 1], ["vs1r_v", "vx", 1]])

    for i in (0, 1):
        generate_base(LdStTwoRegOp, [["vle8_v", "vx", i], ["vle16_v", "vx", i],
                                     ["vle32_v", "vx", i], ["vle64_v", "vx", i],
                                     ["vse8_v", "vx", i], ["vse16_v", "vx", i],
                                     ["vse32_v", "vx", i], ["vse64_v", "vx", i],
                                     ["vle8ff_v", "vx", i], ["vle16ff_v", "vx", i],
                                     ["vle32ff_v", "vx", i], ["vle64ff_v", "vx", i]])

    print_op(LdStThreeoRegOp)
    for i in (0, 1):
        generate_base(LdStThreeoRegOp, [["vlse8_v", "vxx", i], ["vlse16_v", "vxx", i],
                                        ["vlse32_v", "vxx", i], ["vlse64_v", "vxx", i],
                                        ["vsse8_v", "vxx", i], ["vsse16_v", "vxx", i],
                                        ["vsse32_v", "vxx", i], ["vsse64_v", "vxx", i]])

    generate(ThreeVecRegD21VOp, [["vfmerge_vfm", "vfv", 1], ["vmerge_vxm", "vxv", 1],
                                 ["vmerge_vvm", "vvv", 1], ["vsbc_vxm", "vxv", 1],
                                 ["vsbc_vvm", "vvv", 1], ["vadc_vxm", "vxv", 1],
                                 ["vadc_vvm", "vvv", 1], ["vmadc_vxm", "vxv", 1],
                                 ["vmadc_vvm", "vvv", 1], ["vmsbc_vxm", "vxv", 1],
                                 ["vmsbc_vvm", "vvv", 1]])

    print_op(ThreeVecRegD12Op)
    for i in (0, 1):
        generate_base(ThreeVecRegD12Op, [["vfwnmsac_vf", "vfv", i], ["vfwnmsac_vv", "vvv", i],
                                         ["vfwmsac_vf", "vfv", i], ["vfwmsac_vv", "vvv", i],
                                         ["vfwnmacc_vf", "vfv", i], ["vfwnmacc_vv", "vvv", i],
                                         ["vfwmacc_vf", "vfv", i], ["vfwmacc_vv", "vvv", i],
                                         ["vfnmsub_vf", "vfv", i], ["vfnmsub_vv", "vvv", i],
                                         ["vfmsub_vf", "vfv", i], ["vfmsub_vv", "vvv", i],
                                         ["vfnmadd_vf", "vfv", i], ["vfnmadd_vv", "vvv", i],
                                         ["vfmadd_vf", "vfv", i], ["vfmadd_vv", "vvv", i],
                                         ["vfnmsac_vf", "vfv", i], ["vfnmsac_vv", "vvv", i],
                                         ["vfmsac_vf", "vfv", i], ["vfmsac_vv", "vvv", i],
                                         ["vfmacc_vf", "vfv", i], ["vfmacc_vv", "vvv", i],
                                         ["vfnmacc_vf", "vfv", i], ["vfnmacc_vv", "vvv", i],
                                         ["vwmaccsu_vx", "vxv", i], ["vwmaccsu_vv", "vvv", i],
                                         ["vwmacc_vx", "vxv", i], ["vwmacc_vv", "vvv", i],
                                         ["vwmaccu_vx", "vxv", i], ["vwmaccu_vv", "vvv", i],
                                         ["vwmaccus_vx", "vxv", i],
                                         ["vnmsub_vx", "vxv", i], ["vnmsub_vv", "vvv", i],
                                         ["vmadd_vx", "vxv", i], ["vmadd_vv", "vvv", i],
                                         ["vnmsac_vx", "vxv", i], ["vnmsac_vv", "vvv", i],
                                         ["vmacc_vx", "vxv", i], ["vmacc_vv", "vvv", i]])

    print_op(ResSubVecRegOp)
    for i in (0, 1):
        generate_base(ResSubVecRegOp, [["vrsub_vx", "vxv", i]])

    print_op(ThreeVecRegD21Op)
    for i in (0, 1):
        generate_base(ThreeVecRegD21Op,[["vrgather_vx", "vxv", i], ["vrgather_vv", "vvv", i],
                                        ["vslide1down_vx", "vxv", i], ["vslidedown_vx", "vxv", i],
                                        ["vslide1up_vx", "vxv", i], ["vslideup_vx", "vxv", i],
                                        ["vfwredsum_vs", "vvv", i], ["vfwredosum_vs", "vvv", i],
                                        ["vfredsum_vs", "vvv", i], ["vfredosum_vs", "vvv", i],
                                        ["vfredmin_vs", "vvv", i], ["vfredmax_vs", "vvv", i],
                                        ["vredsum_vs", "vvv", i], ["vredand_vs", "vvv", i],
                                        ["vredor_vs", "vvv", i], ["vredxor_vs", "vvv", i],
                                        ["vredminu_vs", "vvv", i], ["vredmin_vs", "vvv", i],
                                        ["vredmaxu_vs", "vvv", i], ["vredmax_vs", "vvv", i],
                                        ["vwredsumu_vs", "vvv", i], ["vwredsum_vs", "vvv", i],
                                        ["vmfge_vf", "vfv", i], ["vmfgt_vf", "vfv", i],
                                        ["vmfle_vf", "vfv", i], ["vmfle_vv", "vvv", i],
                                        ["vmflt_vf", "vfv", i], ["vmflt_vv", "vvv", i],
                                        ["vmfne_vf", "vfv", i], ["vmfne_vv", "vvv", i],
                                        ["vmfeq_vf", "vfv", i], ["vmfeq_vv", "vvv", i],
                                        ["vfslide1down_vf", "vfv", i], ["vfslide1up_vf", "vfv", i],
                                        ["vfsgnjx_vf", "vfv", i], ["vfsgnjx_vv", "vvv", i],
                                        ["vfsgnjn_vf", "vfv", i], ["vfsgnjn_vv", "vvv", i],
                                        ["vfsgnj_vf", "vfv", i], ["vfsgnj_vv", "vvv", i],
                                        ["vfmax_vf", "vfv", i], ["vfmax_vv", "vvv", i],
                                        ["vfmin_vf", "vfv", i], ["vfmin_vv", "vvv", i],
                                        ["vfwmul_vf", "vfv", i], ["vfwmul_vv", "vvv", i],
                                        ["vfdiv_vf", "vfv", i], ["vfdiv_vv", "vvv", i],
                                        ["vfmul_vf", "vfv", i], ["vfmul_vv", "vvv", i],
                                        ["vfrdiv_vf", "vfv", i],
                                        ["vfwsub_wf", "vfv", i], ["vfwsub_wv", "vvv", i],
                                        ["vfwsub_vf", "vfv", i], ["vfwsub_vv", "vvv", i],
                                        ["vfwadd_wf", "vfv", i], ["vfwadd_wv", "vvv", i],
                                        ["vfwadd_vf", "vfv", i], ["vfwadd_vv", "vvv", i],
                                        ["vfsub_vf", "vfv", i], ["vfsub_vv", "vvv", i],
                                        ["vfadd_vf", "vfv", i], ["vfadd_vv", "vvv", i],
                                        ["vfrsub_vf", "vfv", i],
                                        ["vnclip_wx", "vxv", i], ["vnclip_wv", "vvv", i],
                                        ["vnclipu_wx", "vxv", i], ["vnclipu_wv", "vvv", i],
                                        ["vssra_vx", "vxv", i], ["vssra_vv", "vvv", i],
                                        ["vssrl_vx", "vxv", i], ["vssrl_vv", "vvv", i],
                                        ["vsmul_vx", "vxv", i], ["vsmul_vv", "vvv", i],
                                        ["vasubu_vx", "vxv", i], ["vasubu_vv", "vvv", i],
                                        ["vasub_vx", "vxv", i], ["vasub_vv", "vvv", i],
                                        ["vaaddu_vx", "vxv", i], ["vaaddu_vv", "vvv", i],
                                        ["vaadd_vx", "vxv", i], ["vaadd_vv", "vvv", i],
                                        ["vssub_vx", "vxv", i], ["vssub_vv", "vvv", i],
                                        ["vssubu_vx", "vxv", i], ["vssubu_vv", "vvv", i],
                                        ["vsadd_vx", "vxv", i], ["vsadd_vv", "vvv", i],
                                        ["vsaddu_vx", "vxv", i], ["vsaddu_vv", "vvv", i],
                                        ["vwmul_vx", "vxv", i], ["vwmul_vv", "vvv", i],
                                        ["vwmulsu_vx", "vxv", i], ["vwmulsu_vv", "vvv", i],
                                        ["vwmulu_vx", "vxv", i], ["vwmulu_vv", "vvv", i],
                                        ["vrem_vx", "vxv", i], ["vrem_vv", "vvv", i],
                                        ["vremu_vx", "vxv", i], ["vremu_vv", "vvv", i],
                                        ["vdiv_vx", "vxv", i], ["vdiv_vv", "vvv", i],
                                        ["vdivu_vx", "vxv", i], ["vdivu_vv", "vvv", i],
                                        ["vmulhsu_vx", "vxv", i], ["vmulhsu_vv", "vvv", i],
                                        ["vmulhu_vx", "vxv", i], ["vmulhu_vv", "vvv", i],
                                        ["vmulh_vx", "vxv", i], ["vmulh_vv", "vvv", i],
                                        ["vmul_vx", "vxv", i], ["vmul_vv", "vvv", i],
                                        ["vmax_vx", "vxv", i], ["vmax_vv", "vvv", i],
                                        ["vmaxu_vx", "vxv", i], ["vmaxu_vv", "vvv", i],
                                        ["vmin_vx", "vxv", i], ["vmin_vv", "vvv", i],
                                        ["vminu_vx", "vxv", i], ["vminu_vv", "vvv", i],
                                        ["vmsgt_vx", "vxv", i],
                                        ["vmsgtu_vx", "vxv", i],
                                        ["vmsle_vx", "vxv", i], ["vmsle_vv", "vvv", i],
                                        ["vmsleu_vx", "vxv", i], ["vmsleu_vv", "vvv", i],
                                        ["vmslt_vx", "vxv", i], ["vmslt_vv", "vvv", i],
                                        ["vmsltu_vx", "vxv", i], ["vmsltu_vv", "vvv", i],
                                        ["vmsne_vx", "vxv", i], ["vmsne_vv", "vvv", i],
                                        ["vmseq_vx", "vxv", i], ["vmseq_vv", "vvv", i],
                                        ["vnsra_wx", "vxv", i], ["vnsra_wv", "vvv", i],
                                        ["vnsrl_wx", "vxv", i], ["vnsrl_wv", "vvv", i],
                                        ["vsra_vx", "vxv", i], ["vsra_vv", "vvv", i],
                                        ["vsrl_vx", "vxv", i], ["vsrl_vv", "vvv", i],
                                        ["vsll_vx", "vxv", i], ["vsll_vv", "vvv", i],
                                        ["vxor_vx", "vxv", i], ["vxor_vv", "vvv", i],
                                        ["vor_vx", "vxv", i], ["vor_vv", "vvv", i],
                                        ["vand_vx", "vxv", i], ["vand_vv", "vvv", i],
                                        ["vwsub_wx", "vxv", i], ["vwsub_wv", "vvv", i],
                                        ["vwsubu_wx", "vxv", i], ["vwsubu_wv", "vvv", i],
                                        ["vwadd_wx", "vxv", i], ["vwadd_wv", "vvv", i],
                                        ["vwaddu_wx", "vxv", i], ["vwaddu_wv", "vvv", i],
                                        ["vwsub_vx", "vxv", i], ["vwsub_vv", "vvv", i],
                                        ["vwsubu_vx", "vxv", i], ["vwsubu_vv", "vvv", i],
                                        ["vwadd_vx", "vxv", i], ["vwadd_vv", "vvv", i],
                                        ["vwaddu_vx", "vxv", i], ["vwaddu_vv", "vvv", i],
                                        ["vsub_vx", "vxv", i], ["vsub_vv", "vvv", i],
                                        ["vadd_vx", "vxv", i], ["vadd_vv", "vvv", i],
                                        ["vcompress_vm", "vvv", 1], ["vmxnor_mm", "vvv", 1],
                                        ["vmornot_mm", "vvv", 1], ["vmnor_mm", "vvv", 1],
                                        ["vmor_mm", "vvv", 1], ["vmxor_mm", "vvv", 1],
                                        ["vmandnot_mm", "vvv", 1], ["vmnand_mm", "vvv", 1],
                                        ["vmand_mm", "vvv", 1]])

    generate(TwoVecRegImmVOp, [["vmadc_vim", "vv", 1], ["vadc_vim", "vv", 1],
                               ["vmerge_vim", "vv", 1]])

    print_op(TwoVecRegImmOp)
    for i in (0, 1):
        generate_base(TwoVecRegImmOp,[["vsadd_vi", "vv", i], ["vsaddu_vi", "vv", i],
                                      ["vmsgt_vi", "vv", i], ["vmsgtu_vi", "vv", i],
                                      ["vmsle_vi", "vv", i], ["vmsleu_vi", "vv", i],
                                      ["vmsne_vi", "vv", i], ["vmseq_vi", "vv", i],
                                      ["vxor_vi", "vv", i], ["vor_vi", "vv", i],
                                      ["vand_vi", "vv", i], ["vadd_vi", "vv", i]])

    print_op(ImmSubOp)
    for i in (0, 1):
        generate_base(ImmSubOp, [["vrsub_vi", "vv", i]])

    print_op(TwoVecRegUnsignedImmOp)
    for i in (0, 1):
        generate_base(TwoVecRegUnsignedImmOp,[["vrgather_vi", "vv", i], ["vslidedown_vi", "vv", i],
                                              ["vslideup_vi", "vv", i], ["vnclip_wi", "vv", i],
                                              ["vnclipu_wi", "vv", i], ["vssra_vi", "vv", i],
                                              ["vssrl_vi", "vv", i], ["vnsra_wi", "vv", i],
                                              ["vnsrl_wi", "vv", i], ["vsra_vi", "vv", i],
                                              ["vsrl_vi", "vv", i], ["vsll_vi", "vv", i]])

    generate(CfgOp, [["vsetvl", "xxx", 1]])

    print "\n    __ bind(forth);"
    outfile.write("forth:\n")

import subprocess
import sys

subprocess.check_call([RISCV64_AS, "-march=rv64gv_zvqmac", "riscv64ops.s", "-o", "riscv64ops.o"])

print
print "/*",
sys.stdout.flush()
subprocess.check_call([RISCV64_OBJDUMP, "-d", "riscv64ops.o"])
print "*/"

subprocess.check_call([RISCV64_OBJCOPY, "-O", "binary", "-j", ".text", "riscv64ops.o", "riscv64ops.bin"])

with open("riscv64ops.bin", "r") as infile:
    bytes = bytearray(infile.read())

    print
    print "  static const unsigned int insns[] ="
    print "  {"

    i = 0
    while i < len(bytes):
         print "    0x%02x%02x%02x%02x," % (bytes[i+3], bytes[i+2], bytes[i+1], bytes[i]),
         i += 4
         if i%16 == 0:
              print
    print "\n  };"
    print "// END  Generated code -- do not edit"
