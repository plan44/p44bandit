/**
 * This postprocessor creates output for the ancient BANDIT 8300 controller
 */

// Include definition of class GCodeBase:
include("GCodeBase.js");

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

    // header / footer before / after output:
    this.header = [
        "[N]&G99", // BANDIT: drive to machine zero
        "[N] [Z_ZERO][Y1][X1]G92", // set position register (set workpiece zero)
        "[N] G90" // absolute programming (G91 would be relative)
    ];
    this.footer = [
        "[N] M2" // legacy end-of-program (instead of M30)
    ];

    // header / footer before / after tool change:
    this.toolHeader = [
        "[N] M6", // Pause (Tool change)
        "[N] [F]" // feed rate
    ];
    this.toolFooter = [];


    // rapid moves:
    // - need representation of X/Y/Z as I/J/K for rapid moves (Bandit does not have G0/G1)
    //                    name,        ID,        always, prefix, decimals,  options
    this.registerVariable("xPosition", "X_RAPID", false,  "I",    "DEFAULT", "DEFAULT");
    this.registerVariable("yPosition", "Y_RAPID", false,  "J",    "DEFAULT", "DEFAULT");
    this.registerVariable("zPosition", "Z_RAPID", false,  "K",    "DEFAULT", "DEFAULT");
    this.rapidMove =                 "[N] [X_RAPID][Y_RAPID]";
    this.rapidMoveZ =                "[N] [Z]"; // no rapid Z moves for now

    // linear moves:
    // - Bandit does not have G1, all X/Y/Z moves are non-rapid linear moves
    this.firstLinearMove =           "[N] [X][Y]";
    this.linearMove =                "[N] [X][Y]";

    // linear lead in / out:
    // these default to this.linearMove:
    this.linearLeadIn =              undefined;
    this.linearLeadOut =             undefined;

    // linear Z moves:
    this.firstLinearMoveZ =          "[N] [Z]";
    this.linearMoveZ =               "[N] [Z]";

    // point moves for drilling:
    this.firstPointMoveZ =           this.firstLinearMoveZ;
    this.pointMoveZ =                this.linearMoveZ;

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
    var banditZZero = this.getGlobalOption("BanditZZero", "100");
    this.banditZZero = Number(banditZZero);

    this.registerVariable("banditZZero", "Z_ZERO", false, "Z", "DEFAULT", "DEFAULT");

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

    var hBoxLayout = new QHBoxLayout(null);
    vBoxLayout.addLayout(hBoxLayout, 0);

    var lZZero = new QLabel(qsTr("Z Nullpunkt:"));
    hBoxLayout.addWidget(lZZero, 0,0);

    var leZZero = new QLineEdit();
    leZZero.editable = true;
    leZZero.objectName = "BanditZZero";
    hBoxLayout.addWidget(leZZero, 0,0);
};

