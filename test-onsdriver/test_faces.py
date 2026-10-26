'''
Faces: a picture with faces is found and marked in the Faces scope.
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
FACE_PNG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'face.png')
GREEN = (111, 207, 134)  # outline of the largest face


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data():
    'A dock with the Faces scope on the dock selection.'
    scopes = [{'type': 'faces', 'label': 'Faces', 'show_label': False, 'use_roi': True, 'target': '',
               'prop': {'target_scale': 2}}]
    return {'docks': [{
        'name': 'Faces', 'toolbar': True, 'active_view': 0,
        'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': []},
        'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
    }]}


class FacesTest(obstest.OBSTest):
    'Face detection in the Faces scope'

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

    def test_faces(self):
        'The face in the picture is outlined'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        cl = self.obs.get_obsws()
        video = cl.send('GetVideoSettings')
        res = cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Face', 'inputKind': 'image_source',
            'inputSettings': {'file': FACE_PNG},
        })
        cl.send('SetSceneItemTransform', {
            'sceneName': 'Scene', 'sceneItemId': res.scene_item_id,
            'sceneItemTransform': {'boundsType': 'OBS_BOUNDS_STRETCH', 'boundsWidth': video.base_width,
                                   'boundsHeight': video.base_height, 'positionX': 0, 'positionY': 0},
        })
        name = [i['inputName'] for i in cl.send('GetInputList').inputs
                if i['inputKind'].startswith(PREFIX + 'faces_source')][0]
        found = 0
        img = None
        for _ in range(60):
            try:
                data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png',
                                                       'imageWidth': 960, 'imageHeight': 150}).image_data
            except Exception:  # pylint: disable=broad-except
                time.sleep(0.1)
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            # the thumbnail is the left 16:9 part of the row
            found = sum(1 for x in range(0, 250) for y in range(0, 150)
                        if all(abs(a - b) < 40 for a, b in zip(img.getpixel((x, y)), GREEN)))
            if found > 20:
                break
            time.sleep(0.1)
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save('screenshots/faces.png')
        notice(f'faces: {found} outline pixels')
        self.assertGreater(found, 20, 'no face was outlined')


if __name__ == '__main__':
    unittest.main()
