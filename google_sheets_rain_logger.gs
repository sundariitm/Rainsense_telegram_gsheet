/*
 * Google Apps Script for ESP8266 Rain Monitor
 *
 * Supports:
 *   ?action=log
 *   ?action=get_state&device=<device_name>
 *
 * Suggested spreadsheet tabs:
 *   - RainLog
 *   - DeviceState
 */

const LOG_SHEET_NAME = 'RainLog';
const STATE_SHEET_NAME = 'DeviceState';

function doGet(e) {
  try {
    const action = (e.parameter.action || 'log').trim();

    if (action === 'get_state') {
      return handleGetState_(e);
    }

    if (action === 'log') {
      return handleLog_(e);
    }

    return textResponse_([
      'status=ERROR',
      'message=unknown_action'
    ]);
  } catch (err) {
    return textResponse_([
      'status=ERROR',
      'message=' + safeString_(err && err.message ? err.message : err)
    ]);
  }
}

function handleLog_(e) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const logSheet = getOrCreateSheet_(ss, LOG_SHEET_NAME, [
    'Server Time',
    'Device',
    'Timestamp',
    'Epoch',
    'State',
    'Raw A0',
    'Current Wetness %',
    'Current Category',
    '10 Min Avg %',
    '10 Min Category',
    '1 Hour Avg %',
    '1 Hour Category',
    'Mode'
  ]);

  const stateSheet = getOrCreateSheet_(ss, STATE_SHEET_NAME, [
    'Device',
    'Last Update Time',
    'Last Timestamp',
    'Last Rain Epoch',
    'Last State',
    'Last Raw A0',
    'Last Current Wetness %',
    'Last Current Category',
    'Last 10 Min Avg %',
    'Last 1 Hour Avg %',
    'Last Mode'
  ]);

  const p = e.parameter;

  const device = safeString_(p.device || 'unknown-device');
  const timestamp = safeString_(p.timestamp || '');
  const epoch = safeString_(p.epoch || '0');
  const state = safeString_(p.state || 'UNKNOWN');
  const raw = safeString_(p.raw || '');
  const current = safeString_(p.current || '');
  const currentCat = safeString_(p.currentCat || '');
  const avg10 = safeString_(p.avg10 || '');
  const avg10Cat = safeString_(p.avg10Cat || '');
  const avg60 = safeString_(p.avg60 || '');
  const avg60Cat = safeString_(p.avg60Cat || '');
  const testMode = safeString_(p.testMode || 'LIVE');

  logSheet.appendRow([
    new Date(),
    device,
    timestamp,
    epoch,
    state,
    raw,
    current,
    currentCat,
    avg10,
    avg10Cat,
    avg60,
    avg60Cat,
    testMode
  ]);

  upsertDeviceState_(stateSheet, {
    device: device,
    lastUpdateTime: new Date(),
    lastTimestamp: timestamp,
    lastRainEpoch: epoch,
    lastState: state,
    lastRaw: raw,
    lastCurrent: current,
    lastCurrentCat: currentCat,
    lastAvg10: avg10,
    lastAvg60: avg60,
    lastMode: testMode
  });

  return textResponse_([
    'status=OK',
    'action=log',
    'device=' + device,
    'lastRainEpoch=' + epoch
  ]);
}

function handleGetState_(e) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const stateSheet = getOrCreateSheet_(ss, STATE_SHEET_NAME, [
    'Device',
    'Last Update Time',
    'Last Timestamp',
    'Last Rain Epoch',
    'Last State',
    'Last Raw A0',
    'Last Current Wetness %',
    'Last Current Category',
    'Last 10 Min Avg %',
    'Last 1 Hour Avg %',
    'Last Mode'
  ]);

  const device = safeString_(e.parameter.device || '');
  const state = findDeviceState_(stateSheet, device);

  if (!state) {
    return textResponse_([
      'status=OK',
      'device=' + device,
      'lastRainEpoch=0',
      'lastState=UNKNOWN',
      'message=device_not_found'
    ]);
  }

  return textResponse_([
    'status=OK',
    'device=' + state.device,
    'lastUpdateTime=' + state.lastUpdateTime,
    'lastTimestamp=' + state.lastTimestamp,
    'lastRainEpoch=' + state.lastRainEpoch,
    'lastState=' + state.lastState,
    'lastRaw=' + state.lastRaw,
    'lastCurrent=' + state.lastCurrent,
    'lastCurrentCategory=' + state.lastCurrentCategory,
    'lastAvg10=' + state.lastAvg10,
    'lastAvg60=' + state.lastAvg60,
    'lastMode=' + state.lastMode
  ]);
}

function getOrCreateSheet_(ss, name, headers) {
  let sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
  }

  if (sheet.getLastRow() === 0) {
    sheet.appendRow(headers);
    sheet.setFrozenRows(1);
  }

  return sheet;
}

function upsertDeviceState_(sheet, data) {
  const lastRow = sheet.getLastRow();

  if (lastRow <= 1) {
    sheet.appendRow([
      data.device,
      data.lastUpdateTime,
      data.lastTimestamp,
      data.lastRainEpoch,
      data.lastState,
      data.lastRaw,
      data.lastCurrent,
      data.lastCurrentCat,
      data.lastAvg10,
      data.lastAvg60,
      data.lastMode
    ]);
    return;
  }

  const values = sheet.getRange(2, 1, lastRow - 1, 11).getValues();
  for (let i = 0; i < values.length; i++) {
    if (String(values[i][0]).trim() === data.device) {
      sheet.getRange(i + 2, 1, 1, 11).setValues([[
        data.device,
        data.lastUpdateTime,
        data.lastTimestamp,
        data.lastRainEpoch,
        data.lastState,
        data.lastRaw,
        data.lastCurrent,
        data.lastCurrentCat,
        data.lastAvg10,
        data.lastAvg60,
        data.lastMode
      ]]);
      return;
    }
  }

  sheet.appendRow([
    data.device,
    data.lastUpdateTime,
    data.lastTimestamp,
    data.lastRainEpoch,
    data.lastState,
    data.lastRaw,
    data.lastCurrent,
    data.lastCurrentCat,
    data.lastAvg10,
    data.lastAvg60,
    data.lastMode
  ]);
}

function findDeviceState_(sheet, device) {
  const lastRow = sheet.getLastRow();
  if (lastRow <= 1) return null;

  const values = sheet.getRange(2, 1, lastRow - 1, 11).getValues();
  for (let i = 0; i < values.length; i++) {
    if (!device || String(values[i][0]).trim() === device) {
      return {
        device: safeString_(values[i][0]),
        lastUpdateTime: safeString_(values[i][1]),
        lastTimestamp: safeString_(values[i][2]),
        lastRainEpoch: safeString_(values[i][3] || '0'),
        lastState: safeString_(values[i][4]),
        lastRaw: safeString_(values[i][5]),
        lastCurrent: safeString_(values[i][6]),
        lastCurrentCategory: safeString_(values[i][7]),
        lastAvg10: safeString_(values[i][8]),
        lastAvg60: safeString_(values[i][9]),
        lastMode: safeString_(values[i][10])
      };
    }
  }

  return null;
}

function textResponse_(lines) {
  return ContentService
    .createTextOutput(lines.join('\n'))
    .setMimeType(ContentService.MimeType.TEXT);
}

function safeString_(value) {
  if (value === null || value === undefined) return '';
  return String(value).replace(/[\r\n]+/g, ' ').trim();
}
