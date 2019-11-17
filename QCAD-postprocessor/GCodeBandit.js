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
    //                    name,                  ID,                     always, prefix, decimals, options
    this.registerVariable("xPosition",           "X_RAPID",              false, "I", "DEFAULT", "DEFAULT");
    this.registerVariable("yPosition",           "Y_RAPID",              false, "J", "DEFAULT", "DEFAULT");
    this.registerVariable("zPosition",           "Z_RAPID",              false, "K", "DEFAULT", "DEFAULT");
    this.rapidMove =                 "[N] [X_RAPID][Y_RAPID]";
    this.rapidMoveZ =                "[N] [Z]";

    // linear moves:
    this.firstLinearMove =           "[N] [X][Y]";
    this.linearMove =                "[N] [X][Y]";

    // linear lead in / out:
    // these default to this.lineadMove:
    this.linearLeadIn = undefined;
    this.linearLeadOut = undefined;

    // linear Z moves:
    this.firstLinearMoveZ =          "[N] [Z]";
    this.linearMoveZ =               "[N] [Z]";

    // point moves for drilling:
    this.firstPointMoveZ =           this.firstLinearMoveZ;
    this.pointMoveZ =                this.linearMoveZ;

    // circular moves:
    this.splitArcsAtQuadrantLines = true; // BANDIT cannot do arcs over quadrant borders
    // - clockwise and counterclockwise are the same (not ambiguous within a quadrant)
    this.firstArcCWMove =            "[N] [X][Y][I][J]";
    this.arcCWMove =                 "[N] [X][Y][I][J]";
    this.firstArcCCWMove =           "[N] [X][Y][I][J]";
    this.arcCCWMove =                "[N] [X][Y][I][J]";


}

// Configuration is derived from GCodeBase:
GCodeBandit.prototype = new GCodeBase();

// Display name shown in user interface:
GCodeBandit.displayName = "G-Code (BANDIT) [mm]";



/**
 * Initializes extra variables
 */
GCodeBandit.prototype.writeFile = function(fileName) {
    // get configured values:
    var banditZZero = this.getGlobalOption("BanditZZero", "100");
    this.banditZZero = Number(banditZZero);

    this.registerVariable("banditZZero",         "Z_ZERO",              false, "Z", "DEFAULT", "DEFAULT");

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


    // label and combo box samples
    /*
    var hBoxLayout = new QHBoxLayout(null);
    vBoxLayout.addLayout(hBoxLayout, 0);

    var lLaserOn = new QLabel(qsTr("LASER on:"));
    hBoxLayout.addWidget(lLaserOn, 0,0);

    var cbLaserOn = new QComboBox();
    cbLaserOn.editable = true;
    cbLaserOn.objectName = "CamLaserOnCode";
    cbLaserOn.addItem("M03");
    cbLaserOn.addItem("S255 M03");
    cbLaserOn.addItem("S255");
    hBoxLayout.addWidget(cbLaserOn, 0,0);

    hBoxLayout = new QHBoxLayout(null);
    vBoxLayout.addLayout(hBoxLayout, 0);

    var lLaserOff = new QLabel(qsTr("LASER off:"));
    hBoxLayout.addWidget(lLaserOff, 0,0);

    var cbLaserOff = new QComboBox();
    cbLaserOff.editable = true;
    cbLaserOff.objectName = "CamLaserOffCode";
    cbLaserOff.addItem("M05");
    cbLaserOff.addItem("S0 M05");
    cbLaserOff.addItem("S0");
    hBoxLayout.addWidget(cbLaserOff, 0,0);
    */
};



/* this is how to ovveride methods (but we don't need it
GCodeBandit.prototype.exportArc = function(arc) {
    debugger; // debugger would halt here, allowing inspection of vars
    CamExporterV2.prototype.exportArc.call(this, arc); // call "inherited"
};
*/

