'''
Test signal source: the HD bars with a 109 % superwhite and a -7 % sub-black patch, sent as 10-bit Y'CbCr like a
capture card. The waveform on it reads the raw signal, so its trace reaches the top and the bottom of the
-7..109 % scale.
'''

import base64
import io
import json
import os
import time
import unittest
from onsdriver import obstest

DOCK_KEY = 'obs-video-analyser-dock'
PREFIX = 'obs-video-analyser.'
SOURCE = 'Test signal'


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data():
    'A dock on the test signal with an exact luma waveform.'
    scopes = [{'type': 'waveform', 'label': 'Waveform', 'show_label': False, 'use_roi': True, 'target': '',
               'prop': {'target_scale': 2, 'components': 0x20, 'trace_curve': 0, 'trace_smooth': 0, 'display': 0,
                        'intensity': 255, 'graticule_lines': 0, 'scale_labels': 0, 'minmax': False,
                        'out_highlight': False, 'hdr_ref': False, 'ext_guides': False}}]
    return {'docks': [{
        'name': 'Test signal', 'toolbar': True, 'active_view': 0,
        'roi-prop': {'target_name': SOURCE, 'interleave': 0, 'target_scale': 2, 'shapes': []},
        'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
    }]}


class TestSignalTest(obstest.OBSTest):
    'The test signal source'

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection.setdefault('modules', {})[DOCK_KEY] = dock_data()
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        self.obs.run()

    def rows(self, cl, name):
        '''The lit rows of the waveform, after it settled. Only the left 60 % of the width: the raw signal label sits
        at the bottom right.'''
        from PIL import Image  # pylint: disable=import-outside-toplevel
        rows, img = [], None
        for _ in range(60):
            time.sleep(0.15)
            try:
                data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png',
                                                       'imageWidth': 320, 'imageHeight': 256}).image_data
            except Exception:  # pylint: disable=broad-except
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            rows = [y for y in range(img.height)
                    if any(sum(img.getpixel((x, y))) > 90 for x in range(0, img.width * 6 // 10, 4))]
            if rows and rows[0] <= 3 and rows[-1] >= 251:
                break
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save(f'screenshots/testsignal-{int(time.time() * 1000)}.png')
        return rows

    def test_bars(self):
        'The bars reach 109 % and -7 % on the raw waveform; the steps reach 109 % but not below 0 %'
        cl = self.obs.get_obsws()
        cl.send('CreateInput', {'sceneName': 'Scene', 'inputName': SOURCE, 'inputKind': PREFIX + 'test_signal',
                                'inputSettings': {'pattern': 0, 'resolution': 1080, 'fps': 25}})
        name = [i['inputName'] for i in cl.send('GetInputList').inputs
                if i['inputKind'].startswith(PREFIX + 'waveform_source')][0]
        rows = self.rows(cl, name)
        filters = [f['filterKind'] for f in cl.send('GetSourceFilterList', {'sourceName': SOURCE}).filters]
        notice(f'test signal bars: filters {filters}; waveform rows {rows[:3]}..{rows[-3:]}')
        self.assertIn(PREFIX + 'raw_input', filters, 'the raw input filter was not added to the test signal')
        self.assertTrue(rows, 'the waveform draws nothing')
        self.assertLessEqual(rows[0], 3, f'109 % superwhite not at the top: row {rows[0]}')
        self.assertGreaterEqual(rows[-1], 251, f'-7 % sub-black not at the bottom: row {rows[-1]}')

        cl.send('SetInputSettings', {'inputName': SOURCE, 'inputSettings': {'pattern': 3}})
        time.sleep(1.0)
        rows = self.rows(cl, name)
        notice(f'test signal steps: waveform rows {rows[:3]}..{rows[-3:]}')
        self.assertLessEqual(rows[0], 3, f'the 109 % step is not at the top: row {rows[0]}')
        self.assertTrue(230 <= rows[-1] <= 246, f'the 0 % step is not at 0 %: row {rows[-1]}')

    def test_avsync(self):
        'The A/V sync scope finds the flash and the beep of the test signal in sync (the big number is green)'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        cl = self.obs.get_obsws()
        cl.send('CreateInput', {'sceneName': 'Scene', 'inputName': SOURCE, 'inputKind': PREFIX + 'test_signal',
                                'inputSettings': {'pattern': 7, 'resolution': 720, 'fps': 25}})
        cl.send('CreateInput', {'sceneName': 'Scene', 'inputName': 'AV sync', 'inputKind': PREFIX + 'avsync_source',
                                'inputSettings': {'avs_source': SOURCE}})
        green = orange = 0
        img = None
        for _ in range(40):
            time.sleep(0.5)
            try:
                data = cl.send('GetSourceScreenshot', {'sourceName': 'AV sync', 'imageFormat': 'png',
                                                       'imageWidth': 400, 'imageHeight': 220}).image_data
            except Exception:  # pylint: disable=broad-except
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            green = orange = 0
            for y in range(50, 92, 2):
                for x in range(100, 300, 2):
                    r, g, b = img.getpixel((x, y))
                    if g > 170 and r < 150 and b < 150:
                        green += 1
                    elif r > 200 and g > 120 and b < 120:
                        orange += 1
            if green > 20 or orange > 20:
                break
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save(f'screenshots/avsync-{int(time.time() * 1000)}.png')
        notice(f'A/V sync scope: {green} green and {orange} orange pixels in the offset')
        self.assertGreater(green, 20, 'the A/V sync scope does not read the test signal as in sync')


if __name__ == '__main__':
    unittest.main()
