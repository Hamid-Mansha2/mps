#!/usr/local/bin/perl -w
# impl.pl.eventgen: GENERATOR FOR impl.h.eventgen
#
# Copyright (C) 1998, 1999, Harlequin Group plc.  All rights reserved.
# $HopeName: MMsrc!eventgen.pl(trunk.10) $
#
# .how: Invoke this script in the src directory.  It works by scanning
# eventdef.h and then creating a file eventgen.h that includes the
# necessary types and macros.
#
# You will need to have eventgen.h claimed, and you should
# remember to check it in afterwards.

$HopeName = '$HopeName: MMsrc!eventgen.pl(trunk.10) $';

%Formats = ();

%Types = (
  "D", "double",
  "S", "EventStringStruct",
  "U", "unsigned",
  "W", "Word",
  "A", "Addr",
  "P", "void *",
	  );


#### Discover formats


open(C, "<eventdef.h") || die "Can't open $_";
while(<C>) {
  if(/RELATION\([^,]*,[^,]*,[^,]*,[^,]*, ([A-Z]+)\)/) { 
    $Formats{$1} = 1 if(!defined($Formats{$1}));
  }
}
close(C);


#### Generate eventgen.h


open(H, ">eventgen.h") || die "Can't open eventgen.h for output";

print H "/* impl.h.eventgen -- Automatic event header
 *
 * \$HopeName\$
 *
 * DO NOT EDIT THIS FILE!
 * This file was generated by", substr($HopeName, 10), "
 */\n\n";


print H "#ifdef EVENT\n\n";


#### Generate structure definitions and accessors


foreach $format ("", sort(keys(%Formats))) {
  $fmt = ($format eq "") ? "0" : $format;
  print H "typedef struct {\n";
  print H "  Word code;\n  Word clock;\n";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      die "String must be at end of format" if($i+1 != length($format));
    }
    if(!defined($Types{$c})) {
      die "Can't find type for format code >$c<.";
    } else {
      print H "  ", $Types{$c}, " \l$c$i;\n";
    }
  }
  print H "} Event${fmt}Struct;\n\n";

  print H "#define EVENT_${fmt}_FIELD_PTR(event, i) \\\n  (";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    print H "((i) == $i) ? (void *)&((event)->\L$fmt.$c\E$i) \\\n   : ";
  }
  print H "NULL)\n\n";
}


#### Generate union type


print H "\ntypedef union {\n  Event0Struct any;\n";

foreach $format (sort(keys(%Formats))) {
  print H "  Event${format}Struct \L$format;\n";
}
print H "} EventUnion;\n\n\n";


#### Generate writer macros


foreach $format ("", sort(keys(%Formats))) {
  $fmt = ($format eq "") ? "0" : $format;

  print H "#define EVENT_$fmt(type";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      print H ", _l$i, _s$i";
    } else {
      print H ", _\l$c$i";
    }
  }
  print H ") \\\n";

  print H "  EVENT_BEGIN(type) \\\n";

  if(($i = index($format, "S")) != -1) {
    print H "    size_t _string_len; \\\n";
  }

  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      print H "    _string_len = (_l$i); \\\n";
      print H "    AVER(_string_len < EventStringLengthMAX); \\\n";
      print H "    EventMould.\L$fmt.s$i.len = "
                 . "(EventStringLen)_string_len; \\\n";
      print H "    mps_lib_memcpy(EventMould.\L$fmt.s$i.str, "
                               . "_s$i, _string_len); \\\n";
    } else {
      print H "    EventMould.\L$fmt.$c$i = (_$c$i); \\\n";
    }
  }

  if(($i = index($format, "S")) != -1) {
    print H "  EVENT_END(type, $fmt, "
                      . "offsetof(Event${fmt}Struct, s$i.str) "
                      . "+ _string_len)\n\n";
  } else {
    print H "  EVENT_END(type, $fmt, "
                      . "sizeof(Event${fmt}Struct))\n\n";
  }
}


#### Generate format codes


$C = 0;
foreach $format ("0", sort(keys(%Formats))) {
  print H "#define EventFormat$format $C\n";
  $C++;
}


#### Generate dummies for non-event varieties


print H "\n#else /* EVENT not */\n\n";


print H "#define EVENT_0(type) NOOP\n";

foreach $format (sort(keys(%Formats))) {
  print H "#define EVENT_$format(type";
  for($i = 0; $i < length($format); $i++) {
    print H ", p$i";
  }
  if(($i = index($format, "S")) != -1) {
    print H ", l$i";
  }
  print H ") NOOP\n";
}


print H "\n#endif /* EVENT */\n";


close(H);
