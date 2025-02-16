import asyncio


async def f1():
    await f2()


async def f2():
    await asyncio.create_task(f3(), name="F3")


async def f3():
    await asyncio.gather(
        *(
            asyncio.create_task(f4(), name="F4_0"),
            asyncio.create_task(f4(), name="F4_1"),
        )
    )


async def f4():
    await f5()


async def f5():
    await asyncio.sleep(2)


async def main():
    await asyncio.create_task(f1(), name="F1")


asyncio.run(main())
