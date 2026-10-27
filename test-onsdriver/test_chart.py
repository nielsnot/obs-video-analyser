'''
Color chart: a picture of the 24 patches is measured, and compared with the published values the
patches come out as drawn (the live half of every patch matches its color).
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
CHART_PNG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'chart.png')
PUBLISHED = [(115, 82, 68), (194, 150, 130), (98, 122, 157), (87, 108, 67), (133, 128, 177), (103, 189, 170),
             (214, 126, 44), (80, 91, 166), (193, 90, 99), (94, 60, 108), (157, 188, 64), (224, 163, 46),
             (56, 61, 150), (70, 148, 73), (175, 54, 60), (231, 199, 31), (187, 86, 149), (8, 133, 161),
             (243, 243, 242), (200, 200, 200), (160, 160, 160), (122, 122, 121), (85, 85, 85), (52, 52, 52)]


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data():
    'A dock with the chart scope on the whole picture, compared with the published values.'
    scopes = [{'type': 'chart', 'label': 'Chart', 'show_label': False, 'use_roi': True, 'target': '',
               'prop': {'target_scale': 2, 'chart_ref_kind': 1}}]
    return {'docks': [{
        'name': 'Chart', 'toolbar': True, 'active_view': 0,
        'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': []},
        'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
    }]}


class ChartTest(obstest.OBSTest):
    'Color chart measurement'

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

    def test_chart(self):
        'Every patch is measured close to its color'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        cl = self.obs.get_obsws()
        video = cl.send('GetVideoSettings')
        res = cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Chart picture', 'inputKind': 'image_source',
            'inputSettings': {'file': CHART_PNG},
        })
        cl.send('SetSceneItemTransform', {
            'sceneName': 'Scene', 'sceneItemId': res.scene_item_id,
            'sceneItemTransform': {'boundsType': 'OBS_BOUNDS_STRETCH', 'boundsWidth': video.base_width,
                                   'boundsHeight': video.base_height, 'positionX': 0, 'positionY': 0},
        })
        name = [i['inputName'] for i in cl.send('GetInputList').inputs
                if i['inputKind'].startswith(PREFIX + 'chart_source')][0]
        worst = 999
        img = None
        for _ in range(60):
            try:
                data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png',
                                                       'imageWidth': 1200, 'imageHeight': 520}).image_data
            except Exception:  # pylint: disable=broad-except
                time.sleep(0.1)
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            # the live half of patch i: the lower part of its cell (cells of 100 x 100 from 8, 8)
            worst = 0
            for i, color in enumerate(PUBLISHED):
                x = int(8 + (i % 6) * 100 + 48)
                y = int(8 + (i // 6) * 100 + 80)
                got = img.getpixel((x, y))
                worst = max(worst, max(abs(a - b) for a, b in zip(got, color)))
            if worst <= 8:
                break
            time.sleep(0.1)
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save('screenshots/chart.png')
        if img:
            samples = [img.getpixel((int(8 + (i % 6) * 100 + 48), int(8 + (i // 6) * 100 + y)))
                       for i in (0, 18, 23) for y in (24, 80)]
            notice(f'chart: patches 1, 19, 24 (reference, live): {samples}; size {img.size}')
        notice(f'chart: largest patch difference {worst} code values')
        self.assertLessEqual(worst, 8, 'the patches are not measured as drawn')


if __name__ == '__main__':
    unittest.main()
