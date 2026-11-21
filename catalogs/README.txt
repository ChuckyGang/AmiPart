================================================================================
                       AmiPart - Translations (Catalogs)
================================================================================

AmiPart can be translated into any language with the normal AmigaOS
locale.library catalog system.  Everything you need is in this drawer; you do
not need the source code or a compiler.

  AmiPart.cd   every text of the program, in English (the catalog
               description - the single source of truth)
  AmiPart.ct   a ready-made translation template: the same texts, laid out
               for you to translate          (in the release archive only)
  ../support/gencat.py
               a small Python 3 tool that checks a translation and turns it
               into the binary AmiPart.catalog file

locale.library exists on AmigaOS 2.1 (Kickstart V38) and later.  On Kickstart
2.04 (V37) AmiPart runs unchanged and shows its built-in English texts.


--------------------------------------------------------------------------------
 MAKING A TRANSLATION, STEP BY STEP
--------------------------------------------------------------------------------
The examples use "deutsch".  Use the name your language has in the Locale
preferences (deutsch, français, italiano, español, svenska, polski, ...).

1. Get a template.
   Copy AmiPart.ct to deutsch.ct - or, from the source tree, make a fresh one:

       python3 ../support/gencat.py ct AmiPart.cd deutsch.ct deutsch

2. Open deutsch.ct in a text editor and set the language in the header:

       ## language deutsch

3. Translate.  Each text looks like this:

       MSG_CANCEL (1//)
       Cancel
       ;

   Change only the line under the MSG_ name:

       MSG_CANCEL (1//)
       Abbrechen
       ;

   Rules - the tool checks the first two for you:
     * Keep every %-placeholder (%s %lu %ld %lx ...): the same ones, in the
       same order.  They are filled in with names and numbers at run time; a
       missing or swapped one makes the program print rubbish or crash.
     * Keep the "|" characters in button rows ("Yes|No" -> "Ja|Nein").  They
       separate the buttons; the order of the buttons must stay the same.
     * Keep \n (a line break) where the text has one, and keep a \n at the
       end of a text that ends with one.
     * Do not touch the MSG_ line or the numbers in brackets.
     * Save the file as ISO-8859-1 (Latin-1), the Amiga character set - NOT
       as UTF-8.  Letters outside Latin-1 cannot be shown.
     * Requesters are sized for the English text.  Keep translations at a
       similar length; break long lines with \n.
     * You do not have to translate everything.  A text you leave in English
       (or delete from the file) is simply shown in English.

   The lines starting with ";" are comments.  They show which part of the
   program the texts belong to (main window, script messages, ...).

4. Check your work at any time:

       python3 ../support/gencat.py check AmiPart.cd deutsch.ct

   It reports how many texts are translated and names every text whose
   placeholders or button separators do not match the English one.

5. Build the catalog:

       python3 ../support/gencat.py catalog AmiPart.cd deutsch.ct AmiPart.catalog

   Nothing is written while the check in step 4 still reports errors.

6. Install it in one of the two places locale.library looks:

       LOCALE:Catalogs/deutsch/AmiPart.catalog      (system-wide)
       PROGDIR:Catalogs/deutsch/AmiPart.catalog     (next to the AmiPart program)

   Choose the language in the Locale preferences and start AmiPart again.

If you would like your translation to ship with AmiPart, send the .ct file
(not the .catalog) - open an issue or a pull request at
https://github.com/ChuckyGang/AmiPart


--------------------------------------------------------------------------------
 WHEN A NEW AMIPART VERSION ADDS TEXTS
--------------------------------------------------------------------------------
New versions add texts.  An older catalog keeps working - the new texts are
shown in English - but you can bring your translation up to date without
starting again:

       python3 ../support/gencat.py update AmiPart.cd deutsch.ct deutsch-new.ct

deutsch-new.ct contains everything you translated before, plus the new texts
in English, each marked with the comment

       ; NEW - not translated yet

Search for "NEW", translate those, then check and build as above.  Texts the
program no longer uses are dropped and listed on screen.

Texts are matched by their MSG_ name, so a translation made for an older
AmiPart.cd always ends up on the right texts.


--------------------------------------------------------------------------------
 WITHOUT PYTHON (ON THE AMIGA)
--------------------------------------------------------------------------------
AmiPart.cd and the .ct files are ordinary CatComp / FlexCat files:

       FlexCat AmiPart.cd NEWCTFILE deutsch.ct
       FlexCat AmiPart.cd deutsch.ct CATALOG AmiPart.catalog

FlexCat does not check placeholders or button separators the way gencat.py
does, so take extra care with those.


--------------------------------------------------------------------------------
 FROM THE SOURCE TREE (make targets)
--------------------------------------------------------------------------------
       make catalog-template LANG=deutsch   create catalogs/deutsch.ct
       make catalog-update   LANG=deutsch   add new texts to catalogs/deutsch.ct
       make catalog-check    LANG=deutsch   report untranslated texts / mistakes
       make catalog          LANG=deutsch   build catalogs/deutsch/AmiPart.catalog


--------------------------------------------------------------------------------
 EDITING AmiPart.cd (developers)
--------------------------------------------------------------------------------
  * ALWAYS append new messages at the END OF THE FILE - never in the middle,
    not even at the end of their section.  Message numbers are given by
    position, and catalogs that are already installed refer to those numbers.
  * Message names must be unique; the build stops on a duplicate.
  * Encode line breaks as \n, tabs as \t, backslash as \\.
  * One logical string per message (use \n for internal newlines).
  * The C build generates ../src/amipart_strings.h from this file (string ids
    plus the built-in English defaults compiled into the program).
================================================================================
