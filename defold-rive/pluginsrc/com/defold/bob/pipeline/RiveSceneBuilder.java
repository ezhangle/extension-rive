// Copyright 2021 The Defold Foundation
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

package com.dynamo.bob.pipeline;

import java.io.ByteArrayOutputStream;
import java.io.IOException;

import com.dynamo.bob.ProtoBuilder;
import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.ProtoParams;
import com.dynamo.bob.Task;
import com.dynamo.bob.pipeline.BuilderUtil;
import com.dynamo.bob.fs.IResource;
import com.dynamo.rive.proto.Rive.RiveSceneDesc;
import com.google.protobuf.Message;

@ProtoParams(srcClass = RiveSceneDesc.class, messageClass = RiveSceneDesc.class)
@BuilderParams(name="RiveScene", inExts=".rivescene", outExt=".rivescenec")
public class RiveSceneBuilder extends ProtoBuilder<RiveSceneDesc.Builder> {

    @Override
    public void build(Task task) throws CompileExceptionError, IOException {

        RiveSceneDesc.Builder builder = getSrcBuilder(task.firstInput());

        builder.setScene(BuilderUtil.replaceExt(builder.getScene(), ".riv", ".rivc"));
        builder.setAtlas(BuilderUtil.replaceExt(builder.getAtlas(), ".atlas", ".a.texturesetc"));

        Message msg = builder.build();
        ByteArrayOutputStream out = new ByteArrayOutputStream(64 * 1024);
        msg.writeTo(out);
        out.close();
        task.output(0).setContent(out.toByteArray());
    }
}
