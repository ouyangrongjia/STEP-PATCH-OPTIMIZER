
var check;
var count;
var browser;
var head;
var tabs;
var tabNames;
var mclists;
var top_level;
var show_all_tab=0;
var show_inline_help=1;
var show_full_inline_help=1;
var enable_empty_param=0;
var disable_ignore_in_future=0;
var top_model;
var enable_rerun;
var mc_rerun_asm_level;
var mc_rerun_drw_level;
var creo_help_url;
var custom_help_url="";
var is_model_modified=0;
var upd_check = new Array;

function mcCheckTop(name, tab, type, sub_type, mult,mcupd)
{
	 this.name = name;
	 this.tab = tab;
	 this.type = type;
	 this.sub_type = sub_type;
	 this.mult = mult;
	 this.mcupd = mcupd;
}

function mcItem(info1, info2,update_message,update_status)
{
	 this.info1 = info1;
	 this.info2 = info2;
	 if (info2==null)
	   this.info2 = ' '
	 this.update_message = update_message;
	 this.update_status = update_status;
}

function mcList(name,id,readfromxml, choice)
{
	 this.name = name;
	 this.id = id;
	 this.readfromxml = readfromxml;
	 this.choice = choice;
}

function mcChoice(choice)
{
	 this.choice = choice;
}

function mcCheck(renderType,mcCheckTop, stat, desc, msg, ans, title1, title2, func, func_str, item_arr)
{
	 this.renderType = renderType;
	 this.mcCheckTop = mcCheckTop;
	 this.stat = stat;
	 this.desc = desc;
	 this.msg = msg;
	 this.ans = ans;
	 this.title1 = title1;
	 this.title2 = title2;
	 this.func = func;
	 this.func_str = func_str;
	 this.item_arr = item_arr;
	 this.upd_status = 0;
}

function mcHead(model_path, model_title, model, model_pro_type, date_title, date, mode_title, mode,
	config_title, config, checked_by_title, checked_by, last_saved_title, last_saved,
	created_title, created, quality_title, quality)
{
	 this.model_path = model_path;
	 this.model_title = model_title;
	 this.model = model;
	 this.model_pro_type = model_pro_type;
	 this.date_title = date_title;
	 this.date = date;
	 this.mode_title = mode_title;
	 this.mode = mode;
	 this.config_title = config_title;
	 this.config = config;
	 this.checked_by_title = checked_by_title;
	 this.checked_by = checked_by;
	 this.last_saved_title = last_saved_title;
	 this.last_saved = last_saved;
	 this.created_title = created_title;
	 this.created = created;
	 this.quality_title = quality_title;
	 this.quality = quality;
}

function asmBOM(model_name, report_link, error_warning, level)
{
	this.model_name = model_name;
	this.report_link = report_link;
	this.error_warning = error_warning;
	this.level = level;
}

check = new Array;
tabs=new Array;
tabNames=new Array;
asmBOMList=new Array;
var mc_dup_search_with_units=0;

browser = "proe";


divtab_height = 180;
check[0] =   new mcCheck( 50 ,new mcCheckTop ("LAYER_DTM_BLANK","FAILED","ACTION", null,"false") ,"WARNING" ,
	"Datum Features on Unblanked Layers" ,"Number of models that fail for check Datum Features on Unblanked Layers" ,"4" ,null, null ,
	"LINK" ,"Go to Model" ,
	new Array(  new mcItem( "CANDIDATE.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLAT-1345.PRT","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-10.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","WARNING" ) ) )
check[1] =   new mcCheck( 50 ,new mcCheckTop ("IMPORT_FEAT","FAILED","ACTION", null,"false") ,"WARNING" ,
	"Imported Features" ,"Number of models that fail for check Imported Features" ,"4" ,null, null ,
	"LINK" ,"Go to Model" ,
	new Array(  new mcItem( "CANDIDATE.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLAT-1345.PRT","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-10.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","WARNING" ) ) )
check[2] =   new mcCheck( 50 ,new mcCheckTop ("ACCURACY_INFO","FAILED","ACTION", null,"false") ,"WARNING" ,
	"Model Accuracy" ,"Number of models that fail for check Model Accuracy" ,"4" ,null, null ,
	"LINK" ,"Go to Model" ,
	new Array(  new mcItem( "CANDIDATE.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLAT-1345.PRT","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-10.ASM","WARNING" ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","WARNING" ) ) )
check[3] =   new mcCheck( 50 ,new mcCheckTop ("GEOM_CHECKS","FAILED","ACTION", null,"false") ,"ERROR" ,
	"Geom Checks" ,"Number of models that fail for check Geom Checks" ,"1" ,null, null ,
	"LINK" ,"Go to Model" ,
	new Array(  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","ERROR" ) ) )

browser = "proe";


divtab_height = 180;
count =  check.length;

head = new mcHead("","Model:","CANDIDATE.ASM",
"ASM",
"Date:","Mon Sep 21 2026 08:02:16","Mode:","Asm BOM Failed Checks",null,null,
"Checked by:","27836",
"Last saved by:","27836 - Pro/E v. 4200 b. 2024063","Creation date:","Sep 21 2026","","" );
tabs[0] = "Failed Checks";
tabNames[0] = "FAILED";

colors = new Array("Yellow","Red","Blue","Grey","Magenta","Cyan","Green","Brown");

 msg_eng = new Array;
 msg_eng[0] = "Enter numerical value for density:";
 msg_eng[1] = "Enter numerical value for model accuracy:";
 msg_eng[2] = "Changing accuracy will cause full regeneration, please confirm the data can be fully regenerated prior to modifying accuracy.";
 msg_eng[3] = "You have entered an illegal value. The model was not updated.";
 msg_eng[4] = "Pl. re-enter.";
 msg_eng[5] = "Parameter Name";
 msg_eng[6] = "Value";
 msg_eng[7] = "Value entered for parameter";
 msg_eng[8] = "Values entered for parameters";
 msg_eng[9] = "No action supported for such items.";
 msg_eng[10] = "You must enter a reason before you can ignore the error.";
 msg_eng[11] = "This option is not valid for data sharing feature.";
 msg_eng[12] = "Please confirm the data can be fully regenerated prior to modifying accuracy.";
 msg_eng[13] = "Invalid value entered for parameter";
 msg_eng[14] = "Invalid value entered for parameters";
 msg_eng[15] = "Cannot display items in report from the big list";
 msg_eng[16] = "This option is not valid for the selected annotation element.";
 msg_eng[17] = "Select an item from the list before you click the button.";
 msg_eng[18] = "Specify a reason for ignoring the feature";
 msg_eng[19] = "Expand the relevant categories to view all errors.";
 msg_eng[20] = "Expand the relevant categories to view all warnings.";
 msg_eng[21] = "Note: .mct files are located here";
 msg_eng[22] = "This model is modified. Save it before you exit Creo.";
 msg_lang = new Array
 msg_lang[0] = "Enter numerical value for density:";
 msg_lang[1] = "Enter numerical value for model accuracy:";
 msg_lang[2] = "Changing accuracy will cause full regeneration, please confirm the data can be fully regenerated prior to modifying accuracy.";
 msg_lang[3] = "You have entered an illegal value. The model was not updated.";
 msg_lang[4] = "Pl. re-enter.";
 msg_lang[5] = "Parameter Name";
 msg_lang[6] = "Value";
 msg_lang[7] = "Value entered for parameter";
 msg_lang[8] = "Values entered for parameters";
 msg_lang[9] = "No action supported for such items.";
 msg_lang[10] = "You must enter a reason before you can ignore the error.";
 msg_lang[11] = "This option is not valid for data sharing feature.";
 msg_lang[12] = "Please confirm the data can be fully regenerated prior to modifying accuracy.";
 msg_lang[13] = "Invalid value entered for parameter";
 msg_lang[14] = "Invalid value entered for parameters";
 msg_lang[15] = "Cannot display items in report from the big list";
 msg_lang[16] = "This option is not valid for the selected annotation element.";
 msg_lang[17] = "Select an item from the list before you click the button.";
 msg_lang[18] = "Specify a reason for ignoring the feature";
 msg_lang[19] = "Expand the relevant categories to view all errors.";
 msg_lang[20] = "Expand the relevant categories to view all warnings.";
 msg_lang[21] = "Note: .mct files are located here";
 msg_lang[22] = "This model is modified. Save it before you exit Creo.";
 btn_eng = new Array;
 btn_eng[0] = "Update";
 btn_eng[1] = "Update To Uppercase";
 btn_eng[2] = "Rename";
 btn_eng[3] = "Rename All";
 btn_eng[4] = "Delete";
 btn_eng[5] = "Delete All";
 btn_eng[6] = "Add";
 btn_eng[7] = "Add All";
 btn_eng[8] = "Convert";
 btn_eng[9] = "Convert All";
 btn_eng[10] = "Highlight";
 btn_eng[11] = "Update All";
 btn_eng[12] = "Remove";
 btn_eng[13] = "Remove All";
 btn_eng[14] = "Go To Model";
 btn_eng[15] = "Correct Value";
 btn_eng[16] = "Search";
 btn_eng[17] = "OK";
 btn_eng[18] = "Cancel";
 btn_lang = new Array
 btn_lang[0] = "Update";
 btn_lang[1] = "Update To Uppercase";
 btn_lang[2] = "Rename";
 btn_lang[3] = "Rename All";
 btn_lang[4] = "Delete";
 btn_lang[5] = "Delete All";
 btn_lang[6] = "Add";
 btn_lang[7] = "Add All";
 btn_lang[8] = "Convert";
 btn_lang[9] = "Convert All";
 btn_lang[10] = "Highlight";
 btn_lang[11] = "Update All";
 btn_lang[12] = "Remove";
 btn_lang[13] = "Remove All";
 btn_lang[14] = "Go To Model";
 btn_lang[15] = "Correct Value";
 btn_lang[16] = "Search";
 btn_lang[17] = "OK";
 btn_lang[18] = "Cancel";
 miscl_eng = new Array;
 miscl_eng[0] = "Relative";
 miscl_eng[1] = "Absolute";
 miscl_eng[2] = "Result";
 miscl_eng[3] = "Check";
 miscl_eng[4] = "no items";
 miscl_eng[5] = "YES";
 miscl_eng[6] = "Solid geometry";
 miscl_eng[7] = "Note";
 miscl_eng[8] = "Note id";
 miscl_eng[9] = "sheet";
 miscl_eng[10] = "Sheet";
 miscl_eng[11] = "line";
 miscl_eng[12] = "Line";
 miscl_eng[13] = "Name";
 miscl_eng[14] = "Type";
 miscl_eng[15] = "Operator";
 miscl_eng[16] = "Value";
 miscl_eng[17] = "LAYER NAME";
 miscl_eng[18] = "LAYER STATUS";
 miscl_eng[19] = "CURRENT STATUS";
 miscl_eng[20] = "NEW STATUS";
 miscl_eng[21] = "ITEM";
 miscl_eng[22] = "Item";
 miscl_eng[23] = "ERROR";
 miscl_eng[24] = "WARNING";
 miscl_eng[25] = "PASS";
 miscl_eng[26] = "INFO";
 miscl_eng[27] = "print report";
 miscl_eng[28] = "failed";
 miscl_eng[29] = "models";
 miscl_eng[30] = "Error";
 miscl_eng[31] = "Warning";
 miscl_eng[32] = "Pass";
 miscl_eng[33] = "Info";
 miscl_eng[34] = "Symbol id";
 miscl_eng[35] = "STRING";
 miscl_eng[36] = "INTEGER";
 miscl_eng[37] = "REAL";
 miscl_eng[38] = "BOOLEAN";
 miscl_eng[39] = "NONE";
 miscl_eng[40] = "NUMBER";
 miscl_eng[41] = "Dimension";
 miscl_eng[42] = "NOT SET";
 miscl_eng[43] = "Success";
 miscl_eng[44] = "Failure";
 miscl_eng[45] = "Plane not found";
 miscl_eng[46] = "Plane orientation different";
 miscl_eng[47] = "Master";
 miscl_eng[48] = "Overview";
 miscl_eng[49] = "Assembly models";
 miscl_eng[50] = "Assembly failed";
 miscl_eng[51] = "Drawing models";
 miscl_eng[52] = "Errors";
 miscl_eng[53] = "Warnings";
 miscl_eng[54] = "Category";
 miscl_eng[55] = "Design intent";
 miscl_eng[56] = "Feature";
 miscl_eng[57] = "Component";
 miscl_eng[58] = "Drawing";
 miscl_eng[59] = "Miscellaneous";
 miscl_eng[60] = "Information";
 miscl_eng[61] = "Update";
 miscl_eng[62] = "Failed checks";
 miscl_eng[63] = "FAILED COMPONENTS";
 miscl_eng[64] = "Select All";
 miscl_eng[65] = "Model name";
 miscl_eng[66] = "Statistics";
 miscl_eng[67] = "RefDimension";
 miscl_eng[68] = "Driving Dimension";
 miscl_eng[69] = "Annotation Dimension";
 miscl_eng[70] = "Geometric Tolerance";
 miscl_eng[71] = "Datum Target";
 miscl_eng[72] = "Symbol";
 miscl_eng[73] = "Datum Feature Symbol";
 miscl_eng[74] = "Surface Finish";
 miscl_eng[75] = "Surface";
 miscl_eng[76] = "Updated";
 miscl_eng[77] = "Rerun";
 miscl_eng[78] = "Creo ModelCHECK help";
 miscl_eng[79] = "Multiple definition";
 miscl_eng[80] = "Incorrect count";
 miscl_eng[81] = "Annotation inactive";
 miscl_eng[82] = "Attachment geometry missing";
 miscl_eng[83] = "Attachemnt simprep excluded";
 miscl_eng[84] = "Basic Settings";
 miscl_eng[85] = "General";
 miscl_eng[86] = "Deprecated";
 miscl_eng[87] = "Folders";
 miscl_eng[88] = "Mode Enable";
 miscl_eng[89] = "Model Update Settings";
 miscl_eng[90] = "ModelCheck Parameter Settings";
 miscl_eng[91] = "Parameter Settings";
 miscl_eng[92] = "Advanced Settings";
 miscl_eng[93] = "Report Settings";
 miscl_eng[94] = "Shape Indexing Settings";
 miscl_eng[95] = "Regeneration Settings";
 miscl_eng[96] = "Custom Settings";
 miscl_eng[97] = "Annotation Feature";
 miscl_eng[98] = "Components";
 miscl_eng[99] = "Configuration";
 miscl_eng[100] = "Datum";
 miscl_eng[101] = "Dependency";
 miscl_eng[102] = "Drawing";
 miscl_eng[103] = "Family Tables";
 miscl_eng[104] = "Features";
 miscl_eng[105] = "Features - Advanced";
 miscl_eng[106] = "Geom Checks - B";
 miscl_eng[107] = "Geom Checks - A";
 miscl_eng[108] = "Information";
 miscl_eng[109] = "Layers";
 miscl_eng[110] = "MBD";
 miscl_eng[111] = "Miscellaneous";
 miscl_eng[112] = "Notes";
 miscl_eng[113] = "Parameters";
 miscl_eng[114] = "Regeneration Related Checks";
 miscl_eng[115] = "Relations";
 miscl_eng[116] = "RuleCheck";
 miscl_eng[117] = "Sheet Metal";
 miscl_eng[118] = "View";
 miscl_eng[119] = "Tolerances";
 miscl_eng[120] = "Geom Checks";
 miscl_eng[121] = "Shape Indexing";
 miscl_eng[122] = "Accuracy";
 miscl_eng[123] = "Datum Related";
 miscl_eng[124] = "Reference Files";
 miscl_eng[125] = "Configuration [Setup]";
 miscl_lang = new Array
 miscl_lang[0] = "Relative";
 miscl_lang[1] = "Absolute";
 miscl_lang[2] = "Result";
 miscl_lang[3] = "Check";
 miscl_lang[4] = "no items";
 miscl_lang[5] = "YES";
 miscl_lang[6] = "Solid geometry";
 miscl_lang[7] = "Note";
 miscl_lang[8] = "Note id";
 miscl_lang[9] = "sheet";
 miscl_lang[10] = "Sheet";
 miscl_lang[11] = "line";
 miscl_lang[12] = "Line";
 miscl_lang[13] = "Name";
 miscl_lang[14] = "Type";
 miscl_lang[15] = "Operator";
 miscl_lang[16] = "Value";
 miscl_lang[17] = "LAYER NAME";
 miscl_lang[18] = "LAYER STATUS";
 miscl_lang[19] = "CURRENT STATUS";
 miscl_lang[20] = "NEW STATUS";
 miscl_lang[21] = "ITEM";
 miscl_lang[22] = "Item";
 miscl_lang[23] = "ERROR";
 miscl_lang[24] = "WARNING";
 miscl_lang[25] = "PASS";
 miscl_lang[26] = "INFO";
 miscl_lang[27] = "print report";
 miscl_lang[28] = "failed";
 miscl_lang[29] = "models";
 miscl_lang[30] = "Error";
 miscl_lang[31] = "Warning";
 miscl_lang[32] = "Pass";
 miscl_lang[33] = "Info";
 miscl_lang[34] = "Symbol id";
 miscl_lang[35] = "STRING";
 miscl_lang[36] = "INTEGER";
 miscl_lang[37] = "REAL";
 miscl_lang[38] = "BOOLEAN";
 miscl_lang[39] = "NONE";
 miscl_lang[40] = "NUMBER";
 miscl_lang[41] = "Dimension";
 miscl_lang[42] = "NOT SET";
 miscl_lang[43] = "Success";
 miscl_lang[44] = "Failure";
 miscl_lang[45] = "Plane not found";
 miscl_lang[46] = "Plane orientation different";
 miscl_lang[47] = "Master";
 miscl_lang[48] = "Overview";
 miscl_lang[49] = "Assembly models";
 miscl_lang[50] = "Assembly failed";
 miscl_lang[51] = "Drawing models";
 miscl_lang[52] = "Errors";
 miscl_lang[53] = "Warnings";
 miscl_lang[54] = "Category";
 miscl_lang[55] = "Design intent";
 miscl_lang[56] = "Feature";
 miscl_lang[57] = "Component";
 miscl_lang[58] = "Drawing";
 miscl_lang[59] = "Miscellaneous";
 miscl_lang[60] = "Information";
 miscl_lang[61] = "Update";
 miscl_lang[62] = "Failed checks";
 miscl_lang[63] = "FAILED COMPONENTS";
 miscl_lang[64] = "Select All";
 miscl_lang[65] = "Model name";
 miscl_lang[66] = "Statistics";
 miscl_lang[67] = "RefDimension";
 miscl_lang[68] = "Driving Dimension";
 miscl_lang[69] = "Annotation Dimension";
 miscl_lang[70] = "Geometric Tolerance";
 miscl_lang[71] = "Datum Target";
 miscl_lang[72] = "Symbol";
 miscl_lang[73] = "Datum Feature Symbol";
 miscl_lang[74] = "Surface Finish";
 miscl_lang[75] = "Surface";
 miscl_lang[76] = "Updated";
 miscl_lang[77] = "Rerun";
 miscl_lang[78] = "Creo ModelCHECK help";
 miscl_lang[79] = "Multiple definition";
 miscl_lang[80] = "Incorrect count";
 miscl_lang[81] = "Annotation inactive";
 miscl_lang[82] = "Attachment geometry missing";
 miscl_lang[83] = "Attachemnt simprep excluded";
 miscl_lang[84] = "Basic Settings";
 miscl_lang[85] = "General";
 miscl_lang[86] = "Deprecated";
 miscl_lang[87] = "Folders";
 miscl_lang[88] = "Mode Enable";
 miscl_lang[89] = "Model Update Settings";
 miscl_lang[90] = "ModelCheck Parameter Settings";
 miscl_lang[91] = "Parameter Settings";
 miscl_lang[92] = "Advanced Settings";
 miscl_lang[93] = "Report Settings";
 miscl_lang[94] = "Shape Indexing Settings";
 miscl_lang[95] = "Regeneration Settings";
 miscl_lang[96] = "Custom Settings";
 miscl_lang[97] = "Annotation Feature";
 miscl_lang[98] = "Components";
 miscl_lang[99] = "Configuration";
 miscl_lang[100] = "Datum";
 miscl_lang[101] = "Dependency";
 miscl_lang[102] = "Drawing";
 miscl_lang[103] = "Family Tables";
 miscl_lang[104] = "Features";
 miscl_lang[105] = "Features - Advanced";
 miscl_lang[106] = "Geom Checks - B";
 miscl_lang[107] = "Geom Checks - A";
 miscl_lang[108] = "Information";
 miscl_lang[109] = "Layers";
 miscl_lang[110] = "MBD";
 miscl_lang[111] = "Miscellaneous";
 miscl_lang[112] = "Notes";
 miscl_lang[113] = "Parameters";
 miscl_lang[114] = "Regeneration Related Checks";
 miscl_lang[115] = "Relations";
 miscl_lang[116] = "RuleCheck";
 miscl_lang[117] = "Sheet Metal";
 miscl_lang[118] = "View";
 miscl_lang[119] = "Tolerances";
 miscl_lang[120] = "Geom Checks";
 miscl_lang[121] = "Shape Indexing";
 miscl_lang[122] = "Accuracy";
 miscl_lang[123] = "Datum Related";
 miscl_lang[124] = "Reference Files";
 miscl_lang[125] = "Configuration [Setup]";
top_level = "1";


top_model = "candidate.a.html";
enable_rerun = 0;
mc_rerun_asm_level = -1;
mc_rerun_drw_level = -1;