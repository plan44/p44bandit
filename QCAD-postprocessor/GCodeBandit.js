/**
 * This postprocessor creates output for the ancient BANDIT 8300 controller
 */

// Include definition of class GCodeBase:
include("GCodeBase.js");


// If set, output will be UNUSABLE by machine, but show debugging info (what primitives are being used)
const zdbg_enable = false;

function zdbg(cmd, name)
{
  if (!zdbg_enable) return cmd;
  var z_dbg = "safety=[ZS] clearance=[ZU] clearancePass=[ZUP] start=[Z_START] end=[Z_END] depth=[ZD] zzero=[Z_ZERO]";
  return cmd + " ## " + name + ": " + z_dbg;
}

function zdbg_arr(name)
{
  if (!zdbg_enable) return [];
  return [ zdbg("##", name) ];
}


// Constructor:
function GCodeBandit(cadDocumentInterface, camDocumentInterface) {
    GCodeBase.call(this, cadDocumentInterface, camDocumentInterface);

    this.decimals = 3;
    this.options = { trailingZeroes:true }; // because integer number w/o decimal point is interpreted as 1/1000
    this.unit = RS.Millimeter;

    this.lineNumber = 1;
    this.lineNumberIncrement = 1;

    this.outputOffsetPath = true; // No G41/G42!

    this.banditZZero = 0; // Z machine home position
    this.banditFastDrill = false; // fast Z moves for drilling

    // header / footer before / after output:
    this.header = [
        zdbg("[N]&G99", "header"), // BANDIT: drive to machine zero
        "[N] [Z_ZERO][Y1][X1]G92", // set position register (set workpiece zero)
        "[N] G90" // absolute programming (G91 would be relative)
    ];
    this.footer = [
        zdbg("[N] M2", "footer") // legacy end-of-program (instead of M30)
    ];

    // header / footer before / after tool change:
    this.toolHeader = [
        zdbg("[N] M6", "toolHeader"), // Pause (Tool change)
        "[N] [F]" // feed rate
    ];


    this.toolFooter = zdbg_arr("toolFooter");

    // header / footer before / after each toolpath (can be multiple contours):
    this.toolpathHeader = zdbg_arr("toolpathHeader");
    this.toolpathFooter = zdbg_arr("toolpathFooter");

    // header / footer before / after each contour in a toolpath:
    this.contourHeader = zdbg_arr("contourHeader");
    this.contourFooter = zdbg_arr("contourFooter");

    // header / footer before / after single Z pass:
    this.singleZPassHeader = zdbg_arr("singleZPassHeader");
    this.singleZPassFooter = zdbg_arr("singleZPassFooter");

    // header / footer before / after mutliple Z passes:
    this.multiZPassHeader = zdbg_arr("multiZPassHeader");
    this.multiZPassFooter = zdbg_arr("multiZPassFooter");

    // header / footer before / after first pass of mutliple Z passes:
    this.zPassFirstHeader = zdbg_arr("zPassFirstHeader");
    this.zPassFirstFooter = zdbg_arr("zPassFirstFooter");

    // header / footer before / after each pass of mutliple Z passes:
    this.zPassHeader = zdbg_arr("zPassHeader");
    this.zPassFooter = zdbg_arr("zPassFooter");

    // footer after last pass of mutliple Z passes:
    this.zPassLastFooter = zdbg_arr("zPassLastFooter");



    // rapid moves:
    // - use I/J/K  prefixes for rapid moves (Bandit does not have G0/G1)
    //                    name,        ID,        always, prefix, decimals,  options
    this.rapidMove =                 "[N] I[X#]J[Y#]";
    this.rapidMoveZ =                "[N] K[Z#]"; // try rapid Z moves now

    // linear moves:
    // - Bandit does not have G1, all X/Y/Z moves are non-rapid linear moves
    this.firstLinearMove =           "[N] [X][Y]";
    this.linearMove =                "[N] [X][Y]";

    // linear lead in / out:
    // these default to this.linearMove:
    this.linearLeadIn =              undefined;
    this.linearLeadOut =             undefined;

    // linear Z moves:
    this.firstLinearMoveZ =          zdbg("[N] [Z]", "firstLinearMoveZ");
    this.linearMoveZ =               zdbg("[N] [Z]", "linearMoveZ");

    // point moves for drilling:
    // Note: default only, will be overridden in writeFile()
    this.firstPointMoveZ =         zdbg("[N] [Z]", "firstPointMoveZ-normal");
    this.pointMoveZ =              zdbg("[N] [Z]", "pointMoveZ-normal");

    // circular moves:
    this.splitArcsAtQuadrantLines = true; // BANDIT cannot do arcs over quadrant borders
    // - clockwise and counterclockwise are the same (not ambiguous within a quadrant)
    this.firstArcCWMove =            "[N] [X][Y][IA][JA]";
    this.arcCWMove =                 this.firstArcCWMove;
    this.firstArcCCWMove =           this.firstArcCWMove;
    this.arcCCWMove =                this.firstArcCWMove;
}

// Configuration is derived from GCodeBase:
GCodeBandit.prototype = new GCodeBase();

// Display name shown in user interface:
GCodeBandit.displayName = "G-Code (BANDIT) [mm]";


GCodeBandit.prototype.postInit = function(dialog) {
    // Note: this happens before global options for export are known
    return GCodeBase.prototype.postInit.call(this, dialog);
}


/**
 * Writes the given line (string + line ending) to the output stream.
 */
GCodeBandit.prototype.writeLine = function(line) {
    // avoid linenumber-only lines under all circumstances
    // (because these are GOTOs in BANDIT)
    // Note: CamExporterV2 does have a check for that, but it does not work (probably due to spaces)
    var rx = new RegExp("^N\\d+\\s*$", "g");
    if (rx.test(line)) {
        //debugger;
        return;
    }
    // call base implemenation of writeLine:
    return GCodeBase.prototype.writeLine.call(this, line);
}


/**
 * Initializes extra variables
 */
GCodeBandit.prototype.writeFile = function(fileName) {
    // get configured values:
    // - zero offset
    this.banditZZero = this.getGlobalOptionFloat("BanditZZero", 100);
    this.registerVariable("banditZZero", "Z_ZERO", false, "Z", "DEFAULT", "DEFAULT");
    // - fast drilling option
    this.banditFastDrill = this.getGlobalOptionBool("BanditFastDrill", "0") === true;
    // sped up point moves for drilling:
    if (this.banditFastDrill) {
      // fast move top surface, slow drill, fast retract
      this.firstPointMoveZ =         [ zdbg("[N] K0.000", "firstPointMoveZ-fastDrilling"), "[N] [Z]" ]; // fast move down to zero, then slow move
      this.pointMoveZ =              zdbg("[N] K[Z#]", "pointMoveZ-fastDrilling");
    }
    else {
      // standard (slow) Z moves for drilling
      this.firstPointMoveZ =         zdbg("[N] [Z]", "firstPointMoveZ-normal");
      this.pointMoveZ =              zdbg("[N] [Z]", "pointMoveZ-normal");
    }
    this.pointMoveZOri = this.pointMoveZ; // Important: update original setting as well
    // call base implemenation of writeFile:
    return GCodeBase.prototype.writeFile.call(this, fileName);
};


/**
 * Adds controls to the postprocessor configuration dialog
 */
GCodeBandit.prototype.initConfigDialog = function(dialog) {
    // add options for laser on / off:
    var group = dialog.findChild("GroupCustom");
    group.title = qsTr("BANDIT Controller");

    // get QVBoxLayout:
    var vBoxLayout = group.layout();

    // Bandit zero
    var hBoxLayout = new QHBoxLayout(null);
    vBoxLayout.addLayout(hBoxLayout, 0);
    // - Label
    var lZZero = new QLabel(qsTr("Z Nullpunkt:"));
    hBoxLayout.addWidget(lZZero, 0,0);
    // - Editor
    var leZZero = new QLineEdit();
    leZZero.editable = true;
    leZZero.objectName = "BanditZZero";
    hBoxLayout.addWidget(leZZero, 0,0);

    // Bandit fast drilling
    var cbFastDrill = new QCheckBox("Schnelle Z-Anfahrt und Rückzug beim Bohren");
    cbFastDrill.checked = false;
    cbFastDrill.objectName = "BanditFastDrill";
    vBoxLayout.addWidget(cbFastDrill, 0,0);
};

