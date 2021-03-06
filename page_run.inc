// #############################################################################
// ############################ Hilfsfunktionen ################################
// #############################################################################

function extract_float(const grbl_str: string; var start_idx: integer; is_dotsep: Boolean): Double;
var i: Integer;
  my_str: string;
  my_Settings: TFormatSettings;
begin
  my_str:= '';
  result:= 0;
  if start_idx >= length(grbl_str) then
    exit;
  while grbl_str[start_idx] < #33 do
    inc(start_idx);
  for i:= start_idx to length(grbl_str) do begin
    if grbl_str[i] in ['0'..'9', '+', '-', ',', '.'] then
      my_str:= my_str + grbl_str[i]
    else
      break;
  end;
  start_idx:= i+1;
  If is_dotsep then begin
    my_Settings.DecimalSeparator:= '.';
    result:= StrToFloat(my_str, my_Settings);
  end else
    result:= StrToFloat(my_str);
end;

function extract_int(const grbl_str: string; var start_idx: integer): Integer;
var i: Integer;
  my_str: string;
begin
  my_str:= '';
  while grbl_str[start_idx] < #33 do
    inc(start_idx);
  for i:= start_idx to length(grbl_str) do begin
    if grbl_str[i] in ['0'..'9', '+', '-'] then
      my_str:= my_str + grbl_str[i]
    else
      break;
  end;
  start_idx:= i+1;
  result:= StrToInt(my_str);
end;

function pen_description(const pen: Integer): String;
var my_str: string;
  my_tooltip: Integer;
begin
  if pen < 0 then begin
    result:= '(undefined)';
    exit;
  end;
  my_tooltip:= job.pens[pen].tooltip;
  if job.pens[pen].shape = drillhole then
    my_tooltip:= 6;
  my_str:= 'Tool #' + IntToStr(pen)+ ': ' + FormatFloat('0.00',job.pens[pen].diameter) + ' mm'
    + #32 + ToolTipArray[my_tooltip];
  result:= my_str;
end;

procedure spindle_on_off(const switch_on: Boolean);
begin
  if SpindleRunning = switch_on then
    exit;
  WaitForIdle;
  if switch_on then begin
    Form1.Memo1.lines.add('Spindle ON, acceleration wait '+ IntToStr(job.spindle_wait) + ' sec');
    SendSingleCommandStr('M3');
  end else begin
    Form1.Memo1.lines.add('Spindle OFF, brake wait '+ IntToStr(job.spindle_wait div 2) + ' sec');
    SendSingleCommandStr('M5');
  end;
  if isGrblActive then begin
    if switch_on then
      mdelay(job.spindle_wait * 1000)  // Spindel-Hochlaufzeit
    else
      mdelay(job.spindle_wait * 500)  // Spindel-Bremszeit
  end else
    mdelay(500);  // Simulation
  SpindleRunning:= switch_on;
end;

function machine_busy_msg: Boolean;
begin
  if MachineState <> idle then begin
    Form1.Memo1.lines.add('');
    Form1.Memo1.lines.add('WARNING: Machine not idle, command ignored');
    PlaySound('SYSTEMHAND', 0, SND_ASYNC);
  end;
  result:= MachineState <> idle;
end;

// #############################################################################
// ################################ Z PROBING ##################################
// #############################################################################

// Straight Probe Command G38.2 Z-20 F100   -- Z in Maschinenkoordinaten!
// Wenn #define MESSAGE_PROBE_COORDINATES:
// Bei Erreichen des Z-Kontakts:
// [PRB:0.000,0.000,-10.553:1]  -- Maschinenkoordinaten!
// ok
// oder im Fehlerfall:
// ALARM: Probe fail
// [PRB:0.000,0.000,-10.553:0]
// ok
// Wenn MESSAGE_PROBE_COORDINATES abgeschaltet:
// G38.2 Z-1 F100
// ok -- wenn angekommen und gestoppt
// oder im Fehlerfall:
// ALARM: Probe fail
// ok
// ?<Alarm,MPos:0.000,0.000,-1.001,WPos:0.000,0.000,-1.001>

function extract_probe_pos(my_line: String): Double;
// Z Messtaster-Wert holen. Achtung: DisableStatus vorher ausf�hren!
// [PRB:0.000,0.000,-10.553:1]  -- Maschinenkoordinaten!
// T_parseReturnType = (p_none, p_endofline, p_letters, p_number);
var
  my_pos: Integer;
  my_val: Double;
  my_str: String;
begin
  result:= 0;
  my_pos:= pos('PRB:',my_line);
  if my_pos > 0 then begin
    my_pos:= my_pos + 4;
    ParseLine(my_pos, my_line, my_val, my_str); // X-Wert
    ParseLine(my_pos, my_line, my_val, my_str); // Y-Wert
    if ParseLine(my_pos, my_line, my_val, my_str) = p_number then
      result:= my_val;
  end;
end;


function probe_z: double;
var my_zval1, my_zval2: Double;
  pos_changed: Boolean;
  my_cmd, my_response: String;
begin
// von aktueller Position ausgehend Z nach unten fahren.
// Stoppt wenn Kontakt erreicht.
// Untere Z-Position: Travel Z minus L�ngensensor-H�he,
// verhindert, dass das Werkzeug in den Tisch rammt
// nach Stopp durch Kontakt steht Maschinenposition in grbl_mpos.Z
// wird in result �bernommen, Z kehrt danach auf 0 zur�ck
  Form1.Memo1.lines.add('Probing Z (20 mm max.), wait for contact');
  result:= 0;
  if isGrblActive then
    try
      my_zval1:= grbl_mpos.Z;
      my_zval2:= grbl_mpos.Z;
      result:= 0;
      if (MachineState = alarm) then
        exit;
      WaitForIdle;
      grbl_wait_for_timeout(25);
      DisableStatus;

      // wartet auf Probe-Meldung, z.B. [PRB:0.000,0.000,1.492:1]
      my_cmd:= 'G38.2 Z' + FloatToStrDot(my_zval1 - 25) + ' F200';
      my_response:= SendReceive(my_cmd, 0);
      grbl_receiveStr(25); // 'ok'
      if pos('ALARM', ansiuppercase(my_response)) = 0 then
        my_zval1:= extract_probe_pos(my_response)
      else begin
        MachineState:= alarm;
        exit;
      end;


      // 2 mm abheben (falls m�glich) und nochmal langsam
      if (my_zval1 + 2) > 0 then
        exit;  // result ist 0

      SendReceiveAndDwell('G0 G53 Z' + FloatToStrDot(my_zval1 + 2));

      // wartet auf Probe-Meldung, z.B. [PRB:0.000,0.000,1.492:1]
      my_cmd:= 'G38.2 Z' + FloatToStrDot(grbl_mpos.Z - 25) + ' F50';
      my_response:= SendReceive(my_cmd, 0);
      grbl_receiveStr(25); // 'ok'
      if pos('ALARM', ansiuppercase(my_response)) = 0 then
        my_zval2:= extract_probe_pos(my_response)
      else begin
        MachineState:= alarm;
        exit;
      end;

      Form1.Memo1.lines.add('Probe contact at Z = ' + FloatToStr(my_zval2));
      // liegen beide Werte im Toleranzbereich 0,1 mm?
      if CompareValue(my_zval1, my_zval2, 0.1) = 0 then
        result:= my_zval2
      else
        Form1.Memo1.lines.add('ERROR: Probe contact failure - exceed tolerance');

    finally
      if MachineState = alarm then begin
        Form1.Memo1.lines.add('ALARM lock set by GRBL');
        MessageDlg('Probing failed. ALARM LOCK set,'
          + #13 + 'by GRBL - will be cleared automatically.', mtWarning, [mbOK], 0);
        ClearAlarmLock;
      end else if result = 0 then
        MessageDlg('Probing failed, contact trigger exceeds'
          + #13 + 'limit of tolerance. Repeat procedure.', mtWarning, [mbOK], 0);

      grbl_wait_for_timeout(20);
      SendReceiveAndDwell('G0 G53 Z0');  // Z up
      // Workaround f�r GRBL-JOG mit Zero-Eingang als Limit-Switch:
      repeat
        DecodeStatus(grbl_statusStr, pos_changed);
      until MachineState < zero;
      EnableStatus;
    end else if isSimActive then begin
      result:= -10;
      SendReceiveAndDwell('G0 G53 Z0');  // Z up
  end;
end;

function probe_z_fixed: double;
var my_zval: Double;
begin
  result:= 0;
  if (MachineState = alarm) then
    exit;
  // Probe an Fixed-Position anfahren, grbl_mpos.Z merken und zur�ck nach oben
  Form1.Memo1.lines.add('');
  Form1.Memo1.lines.add('Move to reference probe');
  grbl_moveZ(0, true);
  grbl_moveXY(job.probe_x, job.probe_y, true);
  grbl_moveZ(job.probe_z, true);
  grbl_addstr('G4 P0' + #13);
  SendListToGrbl;
  my_zval:= probe_z;
  if my_zval = 0 then
    ResetToolflags
  else
    result:= my_zval;
end;

procedure CancelG43offset;
// WorkZero muss anhand Messklotz oder Kontakth�he gesetzt sein
begin
  if (MachineState = alarm) then
    exit;
  WaitForIdle;
  Form1.Memo1.lines.add('Cancel Tool Length Offset (TLO)');
  SendSingleCommandStr('G49');
end;

procedure NewG43offset(my_delta: Double);
// WorkZero muss anhand Messklotz oder Kontakth�he gesetzt sein
begin
  if (MachineState = alarm) then
    exit;
  WaitForIdle;
  Form1.Memo1.lines.add('Set new Tool Length Offset (TLO) to '+FloatToStrDot(my_delta)+ ' mm');
  SendSingleCommandStr('G43.1 Z'+FloatToStrDot(my_delta));
end;

procedure InvalidateTLCs;
var i: integer;
begin
  for i:= 0 to c_numATCslots do begin
    atcArray[i].TREFok:= false;
    atcArray[i].TLCok:= false;
    atcArray[i].TLCdelta:= 0.0;
  end;
end;

function DoTLCandConfirm(confirm: boolean; atc_idx: Integer): boolean;
// WorkZero muss anhand Messklotz oder Kontakth�he gesetzt sein
// liefert true wenn erfolgreich
// wenn bereits FirstToolReferenced TRUE ist, wird ein neuer L�ngenoffset gesetzt.
// Sonst gilt dieses Tool als Referenz mit Delta = 0.
var my_dlg_result: integer;
begin
  my_dlg_result:= mrOK;
  result:= false;
  if isCancelled then
    exit;
  if (MachineState = alarm) then
    exit;
  result:= true;
  if not Form1.CheckTLCprobe.Checked then
    exit;
  if confirm then
    my_dlg_result:= MessageDlg('Please clear machine to probe Tool Length Offset/Reference (TLC).',
      mtConfirmation, mbOKCancel,0);
  if my_dlg_result = mrOK then begin
    result:= false;
    LEDbusy.Checked:= true;
    CancelG43offset;
    Form1.Memo1.lines.add('Tool Length Offset/Reference (TLC)');
    SendSingleCommandStr('G0 G53 Z0');
    MposOnFixedProbe:= probe_z_fixed; // festen Sensor anfahren
    if (MachineState = alarm) or (MposOnFixedProbe = 0) then
      exit;
    result:= true;
    if atc_idx = FirstToolUsed then begin
      atcArray[FirstToolUsed].TREFok:= true;
      MposOnFixedProbeReference:= MposOnFixedProbe;
      atcArray[FirstToolUsed].TLCref:= MposOnFixedProbeReference;
      atcArray[FirstToolUsed].TLCdelta:= 0;
    end else begin
      atcArray[atc_idx].TLCref:= 0;
      atcArray[atc_idx].TLCok:= true;
      // Differenz zu erstem Werkzeug
      atcArray[atc_idx].TLCdelta:= MposOnFixedProbe - MposOnFixedProbeReference;
      NewG43Offset(atcArray[atc_idx].TLCdelta);
    end;
    UpdateATCsg;
  end;
end;


// #############################################################################
// ######################## R U N  J O B  B U T T O N S ########################
// #############################################################################


function isToolDifferent(pen1, pen2: Integer): Boolean;
// evt. (fast) gleicher Durchmesser und gleiche Form? Dann "false" liefern
begin
  result:= pen1 <> pen2;
  if (CompareValue(job.pens[pen1].diameter, job.pens[pen2].diameter, 0.05) = 0)
    and (job.pens[pen1].tooltip = job.pens[pen2].tooltip) then
      result:= false;
end;

procedure ExitJobMsg;
begin
  // falls wg. speed abgeschaltet
  {$IFnDEF FPC}
  GLSfinalize3Dview;
  {$ENDIF}
  grbl_sendlist.Clear;
  LastToolUsed:= ToolInSpindle;
  spindle_on_off(false);
  drawing_tool_down:= false;
  Form1.Memo1.lines.add('Job ended.');
  Form1.Memo1.lines.add('');
  NeedsRedraw:= true;
  Form1.ProgressBar1.position:= 0;
  Form1.BtnRunJob.enabled:= true;
  Form1.BtnRunTool.enabled:= true;
  Form1.BtnRunGcode.enabled:= true;
  Form1.BtnRunjob.tag:= 0;
  Form1.BtnCancel.tag:= 0;
  SendSingleCommandAndDwell('G0 G53 Z0');
//  UpdateATCsg;
end;

function BitChangeStr(const my_entry: Tfinal): String;
begin
  result:='// Bit change: ' + FormatFloat('0.00', job.pens[my_entry.pen].diameter)
    + ' ' + IntToStr(job.pens[my_entry.pen].tooltip)+' '+ IntToStr(job.pens[my_entry.pen].color);
end;

procedure run_path(my_entry: Tfinal);
// kompletten Milling- oder Drill-Pfad abfahren
var p: Integer;
begin
  if (length(my_entry.millings) > 0) then with Form1 do begin
    LEDbusy.Checked:= true;
    for p:= length(my_entry.millings)-1  downto 0 do begin
      if isCancelled then
        break;
      Application.ProcessMessages;
      Memo1.lines.add('');
      Memo1.lines.add('Run path '+ IntToStr(p));
      Memo1.lines.add('=========================================');
      grbl_addStr(BitChangeStr(my_entry));
      if my_entry.shape = drillhole then
        grbl_drillpath(my_entry.millings[p], my_entry.pen, job.pens[my_entry.pen].offset)
      else
        grbl_millpath(my_entry.millings[p], my_entry.pen, job.pens[my_entry.pen].offset, my_entry.closed);
      SendListToGrbl;
    end;
  end;
end;

procedure TForm1.BtnRunToolClick(Sender: TObject);
// nur einzelnes Werkzeug abarbeiten
var i, atc_idx, my_len, atc_from_pens, my_btn, my_pen: Integer;
  my_entry: Tfinal;
begin
  try
    if machine_busy_msg then
      exit;
    my_len:= length(final_array);
    if my_len < 1 then
       exit;
    if isSimActive then
      ResetSimulation;
    DefaultsGridListToJob;
    PenGridListToJob;
    BtnRunjob.tag:= 1;
    {$IFnDEF FPC}
    Form4.GLLinesPath.Visible:= Form4.CheckToolpathVisible.Checked;
    Form4.GLDummyCubeTool.visible:= true;
    {$ENDIF}
    atc_idx:= sgATC.Row;
    {$IFnDEF FPC}
    GLSsetToolToATCidx(atc_idx);
    {$ENDIF}
    ForceToolPositions(grbl_wpos.x, grbl_wpos.y, grbl_wpos.z);
    Form1.Memo1.lines.add('');
    Memo1.lines.clear;
    my_btn := MessageDlg('Is machine zeroed with selected tool?'
      + #13 + 'Make sure spindle collet is loaded with:'
      + #13 + #13 + pen_description(atcArray[atc_idx].pen)
      + #13 + #13 + 'Click OK to proceed, CANCEL to terminate job.', mtConfirmation, mbOKCancel, 0);
    Memo1.lines.Clear;
    Memo1.lines.add('Job run started.');
    Memo1.lines.add('=========================================');
    if my_btn = mrCancel then
      exit;

    // ATC- bzw. Tool-Liste abarbeiten
    SendSingleCommandStr('G0 G53 Z0');
    if not atcArray[atc_idx].enable then
      exit;
// alle Pfade mit gleicher ATC-Nummer abarbeiten. ATC enth�lt immer die Liste
// der im Job benutzten Werkkzeuge.
    my_pen:= atcArray[atc_idx].pen;
    {$IFnDEF FPC}
    GLSmakeToolArray(job.pens[my_pen].diameter);
    {$ENDIF}
    for i:= 0 to my_len-1 do begin
      my_entry:= final_array[i];
      if not my_entry.enable then
        continue;
      if length(my_entry.millings) = 0 then
        continue;
      atc_from_pens:= job.pens[my_entry.pen].atc;
      if atc_from_pens <> atc_idx then
        continue;
      if isCancelled then
        exit;  // "finally" wird trotzdem ausgef�hrt!
      LEDbusy.Checked:= true;
      spindle_on_off(true);
      run_path(my_entry);
      Memo1.lines.add('Path done.');
    end;
    Memo1.lines.add('Blocks done.');
    WaitForIdle;
// grbl_millpath und grbl_drillpath enden mit job.z_penup, deshalb:
    SendSingleCommandStr('G0 G53 Z0'); // move Z up
    spindle_on_off(false);

    if CheckEndPark.Checked then
      BtnMoveParkClick(nil)
    else begin
      SendSingleCommandStr('G0 X0 Y0'); // Work Zero
    end;
  finally
    ExitJobMsg;
  end;
end;

procedure TForm1.BtnRunJobClick(Sender: TObject);
var i, atc_idx, my_len, atc_from_pens, my_btn, my_pen: Integer;
  my_entry: Tfinal;

begin
  BtnRunJob.enabled:= false;
  BtnRunTool.enabled:= false;
  BtnRunGcode.enabled:= false;
  try
    DefaultsGridListToJob;
    PenGridListToJob;
    if machine_busy_msg then
      exit;
    my_len:= length(final_array);
    if my_len < 1 then
       exit;
    if isSimActive then
      ResetSimulation;
    if CheckUseATC2.checked and (ToolInSpindle <> 1) then begin
      PlaySound('SYSTEMHAND', 0, SND_ASYNC);
      Memo1.lines.add('');
      Memo1.lines.add('ERROR: Tool 1 is not in spindle.');
    end;

    BtnRunjob.tag:= 1;
    {$IFnDEF FPC}
    Form4.GLLinesPath.Visible:= Form4.CheckToolpathVisible.Checked;
    Form4.GLDummyCubeTool.visible:= true;
    GLSsetToolToATCidx(FirstToolUsed);
    {$ENDIF}

    Memo1.lines.clear;
    my_btn := MessageDlg('Is machine zeroed with first tool?'
      + #13 + 'Make sure spindle collet is loaded with:'
      + #13 + #13 + pen_description(atcArray[FirstToolUsed].pen)
      + #13 + #13 + 'Click OK to proceed, CANCEL to terminate job.', mtConfirmation, mbOKCancel, 0);
    Memo1.lines.Clear;
    Memo1.lines.add('Job run started.');
    Memo1.lines.add('=========================================');
    {$IFnDEF FPC}
    GLSneedsRedrawTimeout:= 1;
    GLSneedsATCupdateTimeout:= 1;
    {$ENDIF}
    if my_btn = mrCancel then
      exit;
    if (not atcArray[FirstToolUsed].TREFok) and CheckToolChange.checked then begin
      if not DoTLCandConfirm(false, FirstToolUsed) then
        exit;
    end;

    // ATC- bzw. Tool-Liste abarbeiten
    SendSingleCommandStr('G0 G53 Z0');
    for atc_idx:= 1 to c_numATCslots do begin
      if not atcArray[atc_idx].enable then
        continue;
// alle Pfade mit gleicher ATC-Nummer abarbeiten. ATC enth�lt immer die Liste
// der im Job benutzten Werkzeuge.
      {$IFnDEF FPC}
      GLSsetToolToATCidx(atc_idx);
      {$ENDIF}
      ForceToolPositions(grbl_wpos.x, grbl_wpos.y, grbl_wpos.z);
      my_pen:= atcArray[atc_idx].pen;
      {$IFnDEF FPC}
      GLSmakeToolArray(job.pens[my_pen].diameter);
      {$ENDIF}
      for i:= 0 to my_len-1 do begin
        my_entry:= final_array[i];
        if not my_entry.enable then
          continue;
        if length(my_entry.millings) = 0 then
          continue;
        atc_from_pens:= job.pens[my_entry.pen].atc;
        if atc_from_pens <> atc_idx then
          continue;
        if isCancelled then
          exit;  // "finally" wird trotzdem ausgef�hrt!
        LEDbusy.Checked:= true;
        spindle_on_off(true);
        run_path(my_entry); // Alle Pfade abarbeiten
        Memo1.lines.add('Path done.');
      end;

      if (not isCancelled) and CheckToolChange.checked and (atc_idx < c_numATCslots)
        and atcArray[atc_idx + 1].enable then begin
// Werkzeugwechel nur notwendig, wenn nicht letztes Werkzeug der ATC-Liste
        spindle_on_off(false);
        SendSingleCommandStr('G0 G53 Z0'); // move Z up
        if CheckUseATC2.checked then begin
// automatischer Werkzeugwechsel
          if not ChangeATCtool(atc_idx, atc_idx + 1, true) then
            exit;
        end else
// manueller Werkzeugwechsel
          if not ManualToolchange(atc_idx, atc_idx + 1, true) then
            exit;

        LEDbusy.Checked:= true;
        spindle_on_off(true);
        grbl_moveZ(0, true);
//        grbl_moveXY(0, 0, false);
        grbl_moveXY(final_bounds_mm.mid.x, final_bounds_mm.mid.y, false);
        grbl_moveZ(job.z_penlift, false);
        SendListToGrbl;
      end;
    end; // for loop

    if CheckUseATC2.checked then begin
  // automatischer Werkzeugwechsel
      ChangeATCtool(ToolInSpindle, FirstToolUsed, false);  // erstes Werkzeug aufnehmen, ggf. TRUE
      WorkZeroZdone:= false;
    end;
    if CheckToolChange.checked then
      WorkZeroZdone:= false;
    Memo1.lines.add('Blocks done.');
    WaitForIdle;
// grbl_millpath und grbl_drillpath enden mit job.z_penup, deshalb:
    SendSingleCommandStr('G0 G53 Z0'); // move Z up
    spindle_on_off(false);

    if CheckEndPark.Checked then
      BtnMoveParkClick(nil)
    else begin
      SendSingleCommandStr('G0 X0 Y0'); // Work Zero
    end;
  finally
    ExitJobMsg;
  end;
end;

procedure TForm1.RunGcode(Sender: TObject);
// G-Code-Datei abspielen
var
  my_ReadFile: TextFile;
  my_line, old_line: String;
  pos0, pos1: Integer;
  new_z, z_offs: Double;
  my_Settings: TFormatSettings;

  begin
  DefaultsGridListToJob;
  PenGridListToJob;
  if machine_busy_msg then
    exit;
  try
    if isSimActive then
      ResetSimulation;
    BtnRunjob.tag:= 1;
    Memo1.lines.Clear;
    Memo1.lines.add('G-Code file run started.');
    mdelay(grbl_delay_long);
    {$IFnDEF FPC}
    if Form4.ComboBoxSimType.ItemIndex = 1 then
      Form4.ComboBoxSimType.ItemIndex:= 2;
    Form4.GLLinesPath.Visible:= Form4.CheckToolpathVisible.Checked;
    Form4.GLDummyCubeTool.visible:= true;
    {$ENDIF}
    ForceToolPositions(grbl_wpos.x, grbl_wpos.y, grbl_wpos.z);
    {$IFnDEF FPC}
    GLSneedsRedrawTimeout:= 1;
    GLSneedsATCupdateTimeout:= 1;
    GLSsetToolDia(ComboBoxGdia.ItemIndex+1, ComboBoxGTip.ItemIndex); // Werkzeugform und Farbe
    GLSsetToolColor(clgray);
    {$ENDIF}
    OpenFileDialog.FilterIndex:= 2;
    if not OpenFileDialog.Execute then
      exit;
    my_line:='';
    FileMode := fmOpenRead;
    AssignFile(my_ReadFile, OpenFileDialog.FileName);
    CurrentPen:= 0;
    PendingAction:= lift;
    Reset(my_ReadFile);
    z_offs:= StrToFloatDef(EditZoffs.Text, 0);
    if z_offs <> 0 then
      Memo1.lines.add('Using Z offset' + EditZoffs.Text);
    my_Settings.DecimalSeparator:= '.';
    while not Eof(my_ReadFile) do begin
      if isCancelled then begin
        CloseFile(my_ReadFile);
        exit;
      end;
      LEDbusy.Checked:= true;
      Readln(my_ReadFile, my_line);

      pos0:= pos('Z', my_line);
      if pos0 > 0 then begin
      // Z mit Offset versehen
        if not (pos('G53', my_line) > 0) then begin
          pos1:= pos0+1;
          new_z:= extract_float(my_line, pos1, true); // GCode-Dezimaltrenner
          old_line:= my_line;
          new_z:= new_z + z_offs;
          my_line:= copy(old_line, 0, pos0) + FormatFloat('0.00', new_z, my_Settings)
            + copy(old_line, pos1 - 1, 80);  // bis zum Ende der Zeile
        end;
      end;
      grbl_addStr(my_line);
      if grbl_sendlist.Count > 100 then
        SendListToGrbl;
    end;
    CloseFile(my_ReadFile);
    SendListToGrbl;
    WaitForIdle;
    SendSingleCommandStr('G0 G53 Z0'); // move Z up
    spindle_on_off(false);

    if not isCancelled then begin
      // Immer abschlie�ende Aktion wenn ATC enabled
      if CheckEndPark.Checked and HomingPerformed then
        BtnMoveParkClick(nil)
      else begin
        SendSingleCommandStr('G0 X0 Y0'); // Work Zero
      end;
    end;
  finally
    ExitJobMsg;
  end;
end;


