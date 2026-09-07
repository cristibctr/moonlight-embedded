"""Print matching firmware DWARF types, members, and enums."""
import re
import sys

from elftools.elf.elffile import ELFFile


def value(die, key, default=""):
    attr = die.attributes.get(key)
    result = attr.value if attr else default
    return result.decode(errors="replace") if isinstance(result, bytes) else result


def type_name(die):
    if "DW_AT_type" not in die.attributes:
        return ""
    target = die.get_DIE_from_attribute("DW_AT_type")
    return str(value(target, "DW_AT_name", target.tag))


pattern = re.compile(sys.argv[2], re.I)
with open(sys.argv[1], "rb") as source:
    dwarf = ELFFile(source).get_dwarf_info()
    seen = set()
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            name = value(die, "DW_AT_name")
            child_match = die.tag in ("DW_TAG_enumeration_type", "DW_TAG_structure_type", "DW_TAG_union_type") and any(
                pattern.search(str(value(child, "DW_AT_name")))
                for child in die.iter_children())
            if not pattern.search(str(name)) and not child_match:
                continue
            if die.tag not in ("DW_TAG_structure_type", "DW_TAG_typedef",
                               "DW_TAG_enumeration_type", "DW_TAG_subprogram"):
                continue
            if die.tag == "DW_TAG_typedef":
                die = die.get_DIE_from_attribute("DW_AT_type")
            rows = [f"{name or '<anonymous>'} {die.tag} size={value(die, 'DW_AT_byte_size')}"]
            for child in die.iter_children():
                if child.tag in ("DW_TAG_member", "DW_TAG_enumerator", "DW_TAG_formal_parameter"):
                    rows.append(f"  {value(child, 'DW_AT_name')}: {type_name(child)} "
                                f"offset={value(child, 'DW_AT_data_member_location')} "
                                f"value={value(child, 'DW_AT_const_value')}")
            result = "\n".join(rows)
            if result not in seen:
                seen.add(result)
                print(result)
