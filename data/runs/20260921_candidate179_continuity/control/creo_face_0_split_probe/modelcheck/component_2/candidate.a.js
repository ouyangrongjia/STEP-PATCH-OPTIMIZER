
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
check[0] =   new mcCheck( 19 ,new mcCheckTop ("ACCURACY_INFO","MISC","Miscellaneous","Miscellaneous", null,"0") ,"WARNING" ,
	"Model Accuracy" ,"Model accuracy (Absolute):" ,"0.086599076277" ,null, null ,
	"UPDATE" ,"Modify Accuracy" ,
	new Array(  new mcItem( "0.09" , " " ) ) )
check[1] =   new mcCheck( 0 ,new mcCheckTop ("CHILDREN_EXIST","MISC","Design Intent","Dependency", null,"0") ,"PASS" ,
	"Named Children" ,"Number of features missing specific named children:" ,"0" ,"Parent" ,"Child" ,
	null ,null ,
null)
check[2] =   new mcCheck( 10, new mcCheckTop ("DATUM_RENAME","DATUM","Feature","Datum","YES","0") ,"PASS" ,
	"Datum Rename Specifications" ,"Number of datum rename specifications:" ,"0" ,null, null ,
	"RENAME" ,"Rename Datums" ,
null)
check[3] =   new mcCheck( 18 ,new mcCheckTop ("DTM_AXES_INFO","DATUM","Feature","Datum", null,"0") ,"INFO" ,
	"Datum Axes Information" ,"Datum axes information:" ,"0" ,"Datum" ,null ,
	null ,null ,
null)
check[4] =   new mcCheck( 3 ,new mcCheckTop ("DTM_CSYS_INFO","DATUM","Feature","Datum", null,"0") ,"INFO" ,
	"Datum Coordinate System Information" ,"Datum coordinate system information:" ,"1" ,"Datum" ,"Type" ,
	null ,null ,
	new Array(  new mcItem( "CANDIDATE","DEFAULT"  ) ) )
check[5] =   new mcCheck( 18 ,new mcCheckTop ("DTM_CURVE_INFO","DATUM","Feature","Datum", null,"0") ,"INFO" ,
	"Datum Curve Information" ,"Datum curve information:" ,"0" ,"Datum" ,null ,
	null ,null ,
null)
check[6] =   new mcCheck( 3 ,new mcCheckTop ("DTM_PLANE_INFO","DATUM","Feature","Datum", null,"0") ,"INFO" ,
	"Datum Plane Information" ,"Datum plane information:" ,"0" ,"Datum" ,"Type" ,
	null ,null ,
null)
check[7] =   new mcCheck( 18 ,new mcCheckTop ("DTM_POINT_INFO","DATUM","Feature","Datum", null,"0") ,"INFO" ,
	"Datum Point Information" ,"Datum point information:" ,"0" ,"Datum" ,null ,
	null ,null ,
null)
check[8] =   new mcCheck( 20 ,new mcCheckTop ("FAMILY_INFO","INFO","Design Intent","Family Tables", null,"0") ,"PASS" ,
	"Family Table Info/Verify Status" ,"Family table information:" ,"NO INSTANCE TABLE" ,"Instance|Verified|Succeeded" ,null ,
	null ,null ,
null)
check[9] =   new mcCheck( 3 ,new mcCheckTop ("FEATURE_INFO","FEATURE","Design Intent","Family Tables", null,"0") ,"INFO" ,
	"Feature Information" ,"Number of Feature types:" ,"3" ,"Type" ,"Count" ,
	null ,null ,
	new Array(  new mcItem( "COMPONENT","1" ),
	  new mcItem( "CSYS","1" ),
	  new mcItem( "IMPORT","1" ) ) )
check[10] =   new mcCheck( 0 ,new mcCheckTop ("FILE_SIZE","INFO","Miscellaneous","Information", null,"0") ,"INFO" ,
	"File Size" ,"File size:" ,"67073" ,null , null , null , null ,
null)
check[11] =   new mcCheck( 21, new mcCheckTop ("FT_DEF_VALS","MISC","Design Intent","Family Tables","YES","0") ,"PASS" ,
	"Family Table Default Values" ,"Number of instances having family table items with a value of  * :" ,"0" ,null, null ,
	"UPDATE" ,"Convert instance items to generic values" ,
null)
check[12] =   new mcCheck( 0 ,new mcCheckTop ("GEOMETRIC_TOL","MISC","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Geometric Tolerances" ,"Number of geometric tolerances:" ,"0" ,null , null , null , null ,
null)
check[13] =   new mcCheck( 6 ,new mcCheckTop ("IMPORT_FEAT","FEATURE","Feature","Features", null,"0") ,"WARNING" ,
	"Imported Features" ,"Number of imported features:" ,"1" ,null, null ,
	"HIGHLIGHT|IGNORE|GLBREFVIEW" ,"Highlight|Ignore in future|View References" ,
	new Array(  new mcItem( "Feature id 4" , null  ) ) )
check[14] =   new mcCheck( 22 ,new mcCheckTop ("INSERT_MODE","MISC","Miscellaneous","Information", null,"0") ,"PASS" ,
	"Insert Mode" ,"Insert mode is active:" ,"NO" ,null , null , null , null ,
null)
check[15] =   new mcCheck( 18 ,new mcCheckTop ("LAYER_DISPSTAT","LAYER","Design Intent","Layers", null,"0") ,"PASS" ,
	"Layers with  Isolate  Status" ,"Number of Active Object layers stored with  Isolate  status:" ,"0" ,"Layer" ,null ,
	null ,null ,
null)
check[16] =   new mcCheck( 6, new mcCheckTop ("LAYER_DTM_BLANK","FEATURE","Design Intent","Layers","NO","0") ,"WARNING" ,
	"Datum Features on Unblanked Layers" ,"Number of Datum features that are on unblanked layers:" ,"1" ,null, null ,
	"HIGHLIGHT|IGNORE|GLBREFVIEW" ,"Highlight|Ignore in future|View References" ,
	new Array(  new mcItem( "Feature id 2" ,"DATUM CSYS-CANDIDATE" ) ) )
check[17] =   new mcCheck( 23, new mcCheckTop ("LAYER_EXT_ITEMS","LAYER","Design Intent","Layers","YES","0") ,"PASS" ,
	"Extra items on Layers" ,"Number of non-standard items on layers:" ,"0" ,null, null ,
	"HIGHLIGHT|REMOVE" ,"Highlight|Remove Item from layer" ,
null)
check[18] =   new mcCheck( 15 ,new mcCheckTop ("LAYER_INFO","LAYER","Design Intent","Layers", null,"0") ,"INFO" ,
	"Layer Information" ,"Number of Layers in Active Object:" ,"0" ,null , null , null , null ,
null)
check[19] =   new mcCheck( 10, new mcCheckTop ("LAYER_MOVE","LAYER","Design Intent","Layers","YES","0") ,"INFO" ,
	"Layer Move Specifications" ,"Number of layer move specifications:" ,"0" ,null, null ,
	"RENAME" ,"Rename Layer" ,
null)
check[20] =   new mcCheck( 31, new mcCheckTop ("LAYER_PLACE","LAYER","Design Intent","Layers","YES","0") ,"PASS" ,
	"Layer Place" ,"Number of Active Object layers with items to be added:" ,"0" ,null, null ,
	"PLACE" ,"Add Required Items to Layer" ,
null)
check[21] =   new mcCheck( 30, new mcCheckTop ("LAYER_STATUS","LAYER","Design Intent","Layers","YES","0") ,"PASS" ,
	"Layer Status" ,"Number of Active Object layers that are not correct status:" ,"0" ,null, null ,
	"STATUS" ,"Change Layer Status (from/to)" ,
null)
check[22] =   new mcCheck( 18 ,new mcCheckTop ("LAYOUT_INFO","INFO","Drawing","Drawing", null,"0") ,"INFO" ,
	"Notebook Information" ,"Notebook information:" ,"0" ,"Notebook Name" ,null ,
	null ,null ,
null)
check[23] =   new mcCheck( 0 ,new mcCheckTop ("MEMORY_SPACE","INFO","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Memory Space" ,"Memory space:" ,"NA" ,null , null , null , null ,
null)
check[24] =   new mcCheck( 18 ,new mcCheckTop ("MODEL_NAME","MISC","Miscellaneous","Information", null,"0") ,"PASS" ,
	"Model Name Errors" ,"Model name specification errors:" ,"0" ,"Error" ,null ,
	null ,null ,
null)
check[25] =   new mcCheck( 0 ,new mcCheckTop ("MODEL_NAME_STR","MISC","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Model Name" ,"Model Name:" ,"CANDIDATE.ASM" ,null , null , null , null ,
null)
check[26] =   new mcCheck( 18 ,new mcCheckTop ("NAMED_FEAT","FEATURE","Feature","Features", null,"0") ,"INFO" ,
	"Named Features" ,"Number of named features and the percentage of total features:" ,"1 [33.33%]" ,"Named Features" ,null ,
	null ,null ,
	new Array(  new mcItem( "CANDIDATE" , null  ) ) )
check[27] =   new mcCheck( 0 ,new mcCheckTop ("OVERALL_SIZE","INFO","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Model Overall Size" ,"Model overall size:" ,"3.40 x 2.13 x 2.28" ,null , null , null , null ,
null)
check[28] =   new mcCheck( 3 ,new mcCheckTop ("PARAM_INFO","PARAMETER","Design Intent","Parameters", null,"0") ,"INFO" ,
	"Parameter Information" ,"Parameter information:" ,"7" ,"Parameter" ,"Value" ,
	null ,null ,
	new Array(  new mcItem( "PTC_VAL_IMP_SCORE", "PASS"  ),
	  new mcItem( "PTC_UNITS_LENGTH", "in"  ),
	  new mcItem( "PTC_UNITS_MASS", "lbm"  ),
	  new mcItem( "PTC_UNITS_FORCE", "in lbm / sec^2"  ),
	  new mcItem( "PTC_UNITS_TIME", "sec"  ),
	  new mcItem( "PTC_UNITS_TEMPERATURE", "F"  ),
	  new mcItem( "PTC_UNITS_SYSTEM_TYPE", "Mass length time (MLT)"  ) ) )
check[29] =   new mcCheck( 10, new mcCheckTop ("PARAM_RENAME","PARAMETER","Design Intent","Parameters","YES","0") ,"PASS" ,
	"Parameter Rename Specifications" ,"Number of parameter rename specifications:" ,"0" ,null, null ,
	"RENAME" ,"Rename Parameters" ,
null)
check[30] =   new mcCheck( 2, new mcCheckTop ("PARAMCHECK","PARAMETER","Design Intent","Parameters","YES","0") ,"PASS" ,
	"Parameter Errors" ,"Assembly parameter errors:" ,"0" ,null, null ,
	"UPDATE" ,"Update Parameter" ,
null)
check[31] =   new mcCheck( 0 ,new mcCheckTop ("PRO_VERSION","INFO","Miscellaneous","Information", null,"0") ,"PASS" ,
	"Last saved Version and Build of Creo Parametric" ,"Version and Build of Creo Parametric in which the model was last saved:" ,"4200 2024063 [11.0.0.0]" ,null , null , null , null ,
null)
check[32] =   new mcCheck( 18 ,new mcCheckTop ("REG_FEATURES","FEATURE","Feature","Features", null,"0") ,"INFO" ,
	"Regular Features" ,"Number of resumed features:" ,"3" ,null , null , null , null ,
null)
check[33] =   new mcCheck( 3 ,new mcCheckTop ("RELATION_INFO","RELATION","Design Intent","Relations", null,"0") ,"INFO" ,
	"Relation Information" ,"Relation information:" ,"0" ,null , null ,
	null ,null ,
null)
check[34] =   new mcCheck( 18 ,new mcCheckTop ("SIMPREP_INFO","INFO","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Simplified Representation Information" ,"Simplified representation information:" ,"1" ,"Simp Rep Name" ,null ,
	null ,null ,
	new Array(  new mcItem( "DEFAULT REP" , null  ) ) )
check[35] =   new mcCheck( 18 ,new mcCheckTop ("STARTCHECK","MISC","Miscellaneous","Configuration", null,"0") ,"PASS" ,
	"Start Items" ,"Number of missing start items (assembly):" ,"0" ,"Missing Items" ,null ,
	null ,null ,
null)
check[36] =   new mcCheck( 2, new mcCheckTop ("STARTPARM","PARAMETER","Design Intent","Parameters","YES","0") ,"PASS" ,
	"Missing Parameters" ,"Start Assembly Missing parameters:" ,"0" ,null, null ,
	"ADD" ,"Add Missing Parameters:" ,
null)
check[37] =   new mcCheck( 1, new mcCheckTop ("STARTLAYR","LAYER","Design Intent","Layers","YES","0") ,"PASS" ,
	"Missing Layers" ,"Missing Assembly layers:" ,"0" ,null, null ,
	"ADD" ,"Add Missing Layers:" ,
null)
check[38] =   new mcCheck( 18 ,new mcCheckTop ("SUP_FEATURES","FEATURE","Feature","Features", null,"0") ,"PASS" ,
	"Suppressed Features" ,"Number of suppressed features:" ,"0" ,"Suppressed Features" ,null ,
	null ,null ,
null)
check[39] =   new mcCheck( 3 ,new mcCheckTop ("UDF_INFO","FEATURE","Feature","Features - Advanced", null,"0") ,"INFO" ,
	"User-defined Features" ,"User-defined feature information:" ,"0" ,"Feature" ,"Group" ,
	null ,null ,
null)
check[40] =   new mcCheck( 0 ,new mcCheckTop ("UNITS_LENGTH","MISC","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Model Units for Length" ,"Model units for length:" ,"MM" ,null , null , null , null ,
null)
check[41] =   new mcCheck( 0 ,new mcCheckTop ("UNITS_MASS","MISC","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Model Units for Mass" ,"Model units for mass:" ,"TONNE" ,null , null , null , null ,
null)
check[42] =   new mcCheck( 18 ,new mcCheckTop ("VIEW_INFO","INFO","Miscellaneous","View", null,"0") ,"INFO" ,
	"View Information" ,"View information:" ,"0" ,"View" ,null ,
	null ,null ,
null)
check[43] =   new mcCheck( 18 ,new mcCheckTop ("XSEC_INFO","INFO","Miscellaneous","Information", null,"0") ,"INFO" ,
	"Cross Section Information" ,"Cross section information:" ,"0" ,"Enter NAME for cross-section [Quit]" ,null ,
	null ,null ,
null)
check[44] =   new mcCheck( 29, new mcCheckTop ("EXTERNAL_DEPS","INFO","Design Intent","Dependency","NO","0") ,"PASS" ,
	"External Dependencies Info" ,"Number of external references:" ,"0" ,null, null ,
	"HIGHLIGHT" ,"Highlight" ,
null)
check[45] =   new mcCheck( 3 ,new mcCheckTop ("ASM_BOM","INFO","Component","Components", null,"0") ,"INFO" ,
	"Bill of Material" ,"Bill of material:" ,"4" ,"Name" ,null ,
	null ,null ,
	new Array(  new mcItem( "CANDIDATE.ASM","0"  ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-10.ASM","1[1]"  ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","2[0]"  ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATO-757.PRT","2[1]"  ) ) )
check[46] =   new mcCheck( 6, new mcCheckTop ("ASM_FEATURES","INFO","Feature","Features","NO","0") ,"INFO" ,
	"Assembly Features" ,"Number of assembly features:" ,"1" ,null, null ,
	"HIGHLIGHT" ,"Highlight" ,
	new Array(  new mcItem( "Feature id 4" , null  ) ) )
check[47] =   new mcCheck( 18 ,new mcCheckTop ("BULK_ITEMS","INFO","Component","Components", null,"0") ,"INFO" ,
	"Bulk Items" ,"Number of Bulk Items found:" ,"0" ,"Bulk Name" ,null ,
	null ,null ,
null)
check[48] =   new mcCheck( 18 ,new mcCheckTop ("FRZ_COMPONENTS","MISC","Component","Components", null,"0") ,"PASS" ,
	"Frozen or Suspended Components" ,"Frozen or suspended components exist:" ,"0" ,"Frozen or Suspended Components" ,null ,
	null ,null ,
null)
check[49] =   new mcCheck( 25 ,new mcCheckTop ("GEN_COMPONENTS","INFO","Component","Components", null,"0") ,"PASS" ,
	"Generic Components" ,"Number of components which are generics of a family table:" ,"0" ,null, null ,
	"HIGHLIGHT|GLBREFVIEW" ,"Highlight|View References" ,
null)
check[50] =   new mcCheck( 18 ,new mcCheckTop ("MIS_COMPONENTS","INFO","Component","Components", null,"0") ,"PASS" ,
	"Missing Components" ,"Number of missing components:" ,"0" ,"Component Name" ,null ,
	null ,null ,
null)
check[51] =   new mcCheck( 0 ,new mcCheckTop ("NUM_COMPONENTS","MISC","Component","Components", null,"0") ,"INFO" ,
	"Parts and Sub-assemblies" ,"Number of parts and sub-assemblies:" ,"3" ,null , null , null , null ,
null)
check[52] =   new mcCheck( 18 ,new mcCheckTop ("SUP_COMPONENTS","INFO","Component","Components", null,"0") ,"PASS" ,
	"Suppressed Components" ,"Number of suppressed components:" ,"0" ,"Component Name" ,null ,
	null ,null ,
null)
check[53] =   new mcCheck( 25 ,new mcCheckTop ("PACK_COMPONENTS","INFO","Component","Components", null,"0") ,"PASS" ,
	"Packaged Components" ,"Number of packaged components:" ,"0" ,null, null ,
	"HIGHLIGHT|GLBREFVIEW" ,"Highlight|View References" ,
null)
check[54] =   new mcCheck( 18 ,new mcCheckTop ("UNQ_COMPONENTS","MISC","Component","Components", null,"0") ,"INFO" ,
	"Unique Parts and Sub-assemblies" ,"Number of unique parts and sub-assemblies:" ,"3" ,null , null ,
	null ,null ,
	new Array(  new mcItem( "OPEN_CASCADE_STEP_TRANSLATO-757.PRT" , null  ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-10.ASM" , null  ),
	  new mcItem( "OPEN_CASCADE_STEP_TRANSLATOR-36.PRT" , null  ) ) )
check[55] =   new mcCheck( 28, new mcCheckTop ("CIRCULAR_REFS","INFO","Design Intent","Dependency","NO","0") ,"PASS" ,
	"Circular References" ,"Circular reference loops:" ,"0" ,null, null ,
	"HIGHLIGHT|GLBREFVIEW" ,"Highlight|View References" ,
null)
count =  check.length;

mclists = new Array(	)

head = new mcHead("D:/pyProject/step-patch-optimizer-worktrees/candidate179-continuity-20260921/data/runs/20260921_candidate179_continuity/control/creo_face_0_split_probe/import/","Model:","CANDIDATE.ASM",
"ASM",
"Date:","Mon Sep 21 2026 07:59:51","Mode:","Batch","Config:",
"check/default_checks.mch, start/nostart.mcs, constant/mm.mcn, status/sample_status.mcq","Checked by:","27836",
"Last saved by:","27836 - Pro/E v. 4200 b. 2024063","Creation date:","Sep 21 2026","Status:","green" );
tabs[0] = "All";
tabNames[0] = "ALL";
tabs[1] = "Info";
tabNames[1] = "INFO";
tabs[2] = "Param";
tabNames[2] = "PARAMETER";
tabs[3] = "Layer";
tabNames[3] = "LAYER";
tabs[4] = "Feat";
tabNames[4] = "FEATURE";
tabs[5] = "Relat";
tabNames[5] = "RELATION";
tabs[6] = "Datum";
tabNames[6] = "DATUM";
tabs[7] = "Misc";
tabNames[7] = "MISC";
tabs[8] = "VDA";
tabNames[8] = "VDA";

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
top_level = "0";


asmBOMList[0] = new asmBOM("CANDIDATE.ASM","candidate.a.html","");
asmBOMList[1] = new asmBOM("OPEN_CASCADE_STEP_TRANSLATOR-10.ASM","open_cascade_step_translator-10.a.html","(x1) (0 errors, 3 warnings)1", 1);
asmBOMList[2] = new asmBOM("OPEN_CASCADE_STEP_TRANSLATOR-36.PRT","open_cascade_step_translator-36.p.html","(x1) (2 errors, 3 warnings)1", 2);
asmBOMList[3] = new asmBOM("OPEN_CASCADE_STEP_TRANSLATO-757.PRT","open_cascade_step_translato-757.p.html","(x1) (2 errors, 3 warnings)1", 2);
top_model = "candidate.a.html";
enable_rerun = 0;
mc_rerun_asm_level = -1;
mc_rerun_drw_level = -1;