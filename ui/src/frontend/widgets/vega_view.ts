// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import * as vega from 'vega';
import * as vegaLite from 'vega-lite';

import {Disposable} from '../../base/disposable';
import {shallowEquals} from '../../base/object_utils';
import {SimpleResizeObserver} from '../../base/resize_observer';
import {EngineProxy} from '../../common/engine';
import {getErrorMessage} from '../../common/errors';
import {QueryError} from '../../common/query_result';
import {raf} from '../../core/raf_scheduler';

import {Spinner} from './spinner';

function isVegaLite(spec: unknown): boolean {
  if (typeof spec === 'object') {
    const schema = (spec as {'$schema': unknown})['$schema'];
    if (schema !== undefined && typeof schema === 'string') {
      // If the schema is available use that:
      return schema.includes('vega-lite');
    }
  }
  // Otherwise assume vega-lite:
  return true;
}

export interface VegaViewData {
  [name: string]: any;
}


interface VegaViewAttrs {
  spec: string;
  data: VegaViewData;
  engine?: EngineProxy;
}


// VegaWrapper is in exactly one of these states:
enum Status {
  // Has not visualisation to render.
  Empty,
  // Currently loading the visualisation.
  Loading,
  // Failed to load or render the visualisation. The reson is
  // retrievable via |error|.
  Error,
  // Displaying a visualisation:
  Done,
}


class EngineLoader implements vega.Loader {
  private engine?: EngineProxy;
  private loader: vega.Loader;

  constructor(engine: EngineProxy|undefined) {
    this.engine = engine;
    this.loader = vega.loader();
  }

  async load(uri: string, _options?: any): Promise<string> {
    if (this.engine === undefined) {
      return '';
    }
    const result = this.engine.query(uri);
    try {
      await result.waitAllRows();
    } catch (e) {
      if (e instanceof QueryError) {
        console.error(result.error());
        return '';
      }
    }
    const columns = result.columns();
    const rows: any[] = [];
    for (const it = result.iter({}); it.valid(); it.next()) {
      const row: any = {};
      for (const name of columns) {
        let value = it.get(name);
        if (typeof value === 'bigint') {
          value = Number(value);
        }
        row[name] = value;
      }
      rows.push(row);
    }
    return JSON.stringify(rows);
  }

  sanitize(uri: string, options: any): Promise<{href: string}> {
    return this.loader.sanitize(uri, options);
  }

  http(uri: string, options: any): Promise<string> {
    return this.loader.http(uri, options);
  }

  file(filename: string): Promise<string> {
    return this.loader.file(filename);
  }
}

class VegaWrapper {
  private dom: Element;
  private _spec?: string;
  private _data?: VegaViewData;
  private view?: vega.View;
  private pending?: Promise<vega.View>;
  private _status: Status;
  private _error?: string;
  private _engine?: EngineProxy;

  constructor(dom: Element) {
    this.dom = dom;
    this._status = Status.Empty;
  }

  get status(): Status {
    return this._status;
  }

  get error(): string {
    return this._error ?? '';
  }

  set spec(value: string) {
    if (this._spec !== value) {
      this._spec = value;
      this.updateView();
    }
  }

  set data(value: VegaViewData) {
    if (this._data === value || shallowEquals(this._data, value)) {
      return;
    }
    this._data = value;
    this.updateView();
  }

  set engine(engine: EngineProxy|undefined) {
    this._engine = engine;
  }

  onResize() {
    if (this.view) {
      this.view.resize();
    }
  }

  private updateView() {
    this._status = Status.Empty;
    this._error = undefined;

    // We no longer care about inflight renders:
    if (this.pending) {
      this.pending = undefined;
    }

    // Destroy existing view if needed:
    if (this.view) {
      this.view.finalize();
      this.view = undefined;
    }

    // If the spec and data are both available then create a new view:
    if (this._spec !== undefined && this._data !== undefined) {
      let spec;
      try {
        spec = JSON.parse(this._spec);
      } catch (e) {
        this.setError(e);
        return;
      }

      if (isVegaLite(spec)) {
        try {
          spec = vegaLite.compile(spec, {}).spec;
        } catch (e) {
          this.setError(e);
          return;
        }
      }

      // Create the runtime and view the bind the host DOM element
      // and any data.
      const runtime = vega.parse(spec);
      this.view = new vega.View(runtime, {
        loader: new EngineLoader(this._engine),
      });
      this.view.initialize(this.dom);
      for (const [key, value] of Object.entries(this._data)) {
        this.view.data(key, value);
      }

      const pending = this.view.runAsync();
      pending
          .then(() => {
            this.handleComplete(pending);
          })
          .catch((err) => {
            this.handleError(pending, err);
          });
      this.pending = pending;
      this._status = Status.Loading;
    }
  }

  private handleComplete(pending: Promise<vega.View>) {
    if (this.pending !== pending) {
      return;
    }
    this._status = Status.Done;
    this.pending = undefined;
    raf.scheduleFullRedraw();
  }

  private handleError(pending: Promise<vega.View>, err: unknown) {
    if (this.pending !== pending) {
      return;
    }
    this.pending = undefined;
    this.setError(err);
  }

  private setError(err: unknown) {
    this._status = Status.Error;
    this._error = getErrorMessage(err);
    raf.scheduleFullRedraw();
  }

  dispose() {
    this._data = undefined;
    this._spec = undefined;
    this.updateView();
  }
}

export class VegaView implements m.ClassComponent<VegaViewAttrs> {
  private wrapper?: VegaWrapper;
  private resize?: Disposable;

  oncreate({dom, attrs}: m.CVnodeDOM<VegaViewAttrs>) {
    const wrapper = new VegaWrapper(dom.firstElementChild!);
    wrapper.spec = attrs.spec;
    wrapper.data = attrs.data;
    wrapper.engine = attrs.engine;
    this.wrapper = wrapper;
    this.resize = new SimpleResizeObserver(dom, () => {
      wrapper.onResize();
    });
  }

  onupdate({attrs}: m.CVnodeDOM<VegaViewAttrs>) {
    if (this.wrapper) {
      this.wrapper.spec = attrs.spec;
      this.wrapper.data = attrs.data;
      this.wrapper.engine = attrs.engine;
    }
  }

  onremove() {
    if (this.resize) {
      this.resize.dispose();
      this.resize = undefined;
    }
    if (this.wrapper) {
      this.wrapper.dispose();
      this.wrapper = undefined;
    }
  }

  view(_: m.Vnode<VegaViewAttrs>) {
    return m(
        '.pf-vega-view',
        m(''),
        (this.wrapper?.status === Status.Loading) &&
            m('.pf-vega-view-status', m(Spinner)),
        (this.wrapper?.status === Status.Error) &&
            m('.pf-vega-view-status', this.wrapper?.error ?? 'Error'),
    );
  }
}
