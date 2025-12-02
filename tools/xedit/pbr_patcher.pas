{
  PBR Texture Patch Generator
  ---------------------------
  Reads pbr_output/pbr_map.tsv produced by the patcher, finds records that use
  the listed meshes, creates/recyles TXST records pointing to the PBR textures,
  and assigns them via Alternate Textures (MODS). Outputs an ESL-flagged patch.

  Patch name rule:
    - If current working directory name is "data" (case-insensitive): pbr_patch.esp
    - Otherwise: <dirname>_patch.esp
}

unit userscript;

uses mteElements, mteFunctions, mteBase, wbInterface, SysUtils, Classes, StrUtils;

var
  mappingLines: TStringList;
  meshMap: TStringList;
  txstCache: TStringList;
  patchFile: IInterface;
  mappingPath: string;

function NormalizePath(const s: string): string;
begin
  Result := LowerCase(StringReplace(s, '/', '\', [rfReplaceAll]));
end;

function PatchFileName: string;
var
  dirName: string;
begin
  dirName := ExtractFileName(ExcludeTrailingPathDelimiter(GetCurrentDir));
  if SameText(dirName, 'data') then
    Result := 'pbr_patch.esp'
  else
    Result := dirName + '_patch.esp';
end;

function EnsurePatchFile: IInterface;
begin
  if Assigned(patchFile) then begin
    Result := patchFile;
    exit;
  end;
  patchFile := AddNewFileName(PatchFileName);
  if not Assigned(patchFile) then
    raise Exception.Create('Failed to create patch file');
  SetElementNativeValues(patchFile, 'Record Header\Record Flags\ESL', 1);
  Result := patchFile;
end;

procedure LoadMapping;
var
  i: Integer;
  line, meshKey, value: string;
  parts: TStringList;
begin
  mappingLines := TStringList.Create;
  meshMap := TStringList.Create;
  meshMap.Sorted := True;
  meshMap.Duplicates := dupAccept;
  meshMap.NameValueSeparator := '=';

  if not FileExists(mappingPath) then
    raise Exception.Create('Mapping file not found: ' + mappingPath);

  mappingLines.LoadFromFile(mappingPath);
  if mappingLines.Count = 0 then
    raise Exception.Create('Mapping file is empty: ' + mappingPath);

  parts := TStringList.Create;
  parts.Delimiter := #9;
  parts.StrictDelimiter := True;

  for i := 1 to mappingLines.Count - 1 do begin
    line := mappingLines[i];
    parts.DelimitedText := line;
    if parts.Count < 10 then
      Continue;
    meshKey := NormalizePath(parts[0]);
    // Re-store line without mesh (only shape+slots) as value
    value := Copy(line, Pos(#9, line), Length(line));
    meshMap.Add(meshKey + meshMap.NameValueSeparator + value);
  end;

  parts.Free;
end;

function SlotsKey(const slots: array of string): string;
var
  i: Integer;
begin
  Result := '';
  for i := Low(slots) to High(slots) do begin
    if i > 0 then
      Result := Result + '|';
    Result := Result + slots[i];
  end;
end;

function GetOrCreateTXST(const slots: array of string): IInterface;
var
  key, edid: string;
  idx: Integer;
  txst: IInterface;
  i: Integer;
  texPath: string;
  arr: IInterface;
begin
  key := SlotsKey(slots);
  idx := txstCache.IndexOfName(key);
  if idx <> -1 then begin
    Result := ObjectToElement(txstCache.Objects[idx]);
    exit;
  end;

  edid := 'PBR_TXST_' + IntToHex(txstCache.Count + 1, 4);
  txst := AddNewRecordToFile(EnsurePatchFile, 'TXST');
  SetElementEditValues(txst, 'EDID', edid);

  arr := ElementByPath(txst, 'Textures');
  for i := 0 to 7 do begin
    texPath := '';
    if i <= High(slots) then
      texPath := slots[i];
    if texPath = '' then
      Continue;
    SetElementEditValues(arr, Format('%d', [i]), texPath);
  end;

  txstCache.AddObject(key + '=' + edid, ElementToObject(txst));
  Result := txst;
end;

function FindModelPath(container: IInterface): string;
begin
  Result := GetEditValue(ElementByPath(container, 'MODL'));
  if Result = '' then Result := GetEditValue(ElementByPath(container, 'MOD2 - Model Filename'));
  if Result = '' then Result := GetEditValue(ElementByPath(container, 'MOD3 - Model Filename'));
  if Result = '' then Result := GetEditValue(ElementByPath(container, 'MOD4 - Model Filename'));
  if Result = '' then Result := GetEditValue(ElementByPath(container, 'MOD5 - Model Filename'));
end;

procedure ApplyAlternateTexture(modelContainer: IInterface; const shape: string; txst: IInterface);
var
  mods, entry: IInterface;
  idx: Integer;
  nameVal: string;
begin
  mods := ElementByPath(modelContainer, 'MODS');
  if not Assigned(mods) then
    mods := Add(modelContainer, 'MODS', true);

  entry := nil;
  for idx := 0 to ElementCount(mods) - 1 do begin
    entry := ElementByIndex(mods, idx);
    nameVal := GetElementEditValues(entry, 'Name');
    if nameVal = shape then begin
      Break;
    end else
      entry := nil;
  end;

  if not Assigned(entry) then
    entry := ElementAssign(mods, HighInteger, nil, false);

  SetElementEditValues(entry, 'Name', shape);
  SetElementEditValues(entry, 'New Texture', Name(txst));
end;

procedure ApplyMappingToContainer(rec: IInterface; const containerPath: string; const meshKey: string);
var
  idx, posIdx: Integer;
  value, shape: string;
  parts: TStringList;
  slots: array[0..7] of string;
  txst: IInterface;
  recOverride: IInterface;
  targetContainer: IInterface;
begin
  if not meshMap.Find(meshKey, posIdx) then
    exit;

  recOverride := wbCopyElementToFile(rec, EnsurePatchFile, false, true);
  targetContainer := ElementByPath(recOverride, containerPath);
  if not Assigned(targetContainer) then
    exit;

  parts := TStringList.Create;
  parts.Delimiter := #9;
  parts.StrictDelimiter := True;

  while (posIdx < meshMap.Count) and (meshMap.Names[posIdx] = meshKey) do begin
    value := meshMap.ValueFromIndex[posIdx];
    parts.DelimitedText := meshKey + value; // reattach mesh for consistent indexing
    if parts.Count >= 10 then begin
      shape := parts[1];
      slots[0] := parts[2]; slots[1] := parts[3]; slots[2] := parts[4]; slots[3] := parts[5];
      slots[4] := parts[6]; slots[5] := parts[7]; slots[6] := parts[8]; slots[7] := parts[9];
      txst := GetOrCreateTXST(slots);
      ApplyAlternateTexture(targetContainer, shape, txst);
    end;
    Inc(posIdx);
  end;

  parts.Free;
end;

procedure ProcessRecord(rec: IInterface);
var
  sig: string;
  containers: array[0..4] of IInterface;
  containerPaths: array[0..4] of string;
  i: Integer;
  modelPath, meshKey: string;
begin
  sig := Signature(rec);
  if not (sig in ['STAT','MSTT','ACTI','FURN','DOOR','LIGH','WEAP','ARMO','ARMA','AMMO']) then
    exit;

  containers[0] := ElementByPath(rec, 'Model');
  containers[1] := ElementByPath(rec, 'Male world model');
  containers[2] := ElementByPath(rec, 'Female world model');
  containers[3] := ElementByPath(rec, 'Male 1st Person');
  containers[4] := ElementByPath(rec, 'Female 1st Person');
  containerPaths[0] := 'Model';
  containerPaths[1] := 'Male world model';
  containerPaths[2] := 'Female world model';
  containerPaths[3] := 'Male 1st Person';
  containerPaths[4] := 'Female 1st Person';

  for i := 0 to 4 do begin
    if not Assigned(containers[i]) then
      continue;
    modelPath := FindModelPath(containers[i]);
    if modelPath = '' then
      continue;
    meshKey := NormalizePath(modelPath);
    ApplyMappingToContainer(rec, containerPaths[i], meshKey);
  end;
end;

function Initialize: Integer;
var
  i, j: Integer;
  f, rec: IInterface;
  modelPath: string;
begin
  txstCache := TStringList.Create;
  txstCache.Sorted := True;
  txstCache.Duplicates := dupIgnore;
  txstCache.NameValueSeparator := '=';

  mappingPath := ProgramPath + 'pbr_output\pbr_map.tsv';
  if not FileExists(mappingPath) then
    mappingPath := GetCurrentDir + '\pbr_output\pbr_map.tsv';

  LoadMapping;

  AddMessage('Loaded mapping: ' + IntToStr(mappingLines.Count-1) + ' entries');
  AddMessage('Patch file: ' + PatchFileName);

  for i := 0 to FileCount - 1 do begin
    f := FileByIndex(i);
    if GetFileName(f) = GetFileName(EnsurePatchFile) then
      continue;
    for j := 0 to Pred(ElementCount(f)) do begin
      rec := ElementByIndex(f, j);
      ProcessRecord(rec);
    end;
  end;

  Result := 0;
end;

function Finalize: Integer;
begin
  if Assigned(mappingLines) then mappingLines.Free;
  if Assigned(meshMap) then meshMap.Free;
  if Assigned(txstCache) then txstCache.Free;
  Result := 0;
end;

end.
