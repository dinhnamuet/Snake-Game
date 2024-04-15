#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include "ssd1306.h"

#define BUTTON_UP 1
#define BUTTON_DOWN 0
#define BUTTON_LEFT 3
#define BUTTON_RIGHT 2

const int max_X = OLED_WIDTH / FONT_X - 1;
const int max_Y = OLED_HEIGHT / 8 - 1;

struct ssd1306
{
	struct i2c_client *client;
	uint8_t current_X;
	uint8_t current_Y;
	struct work_struct workqueue;
	struct timer_list my_timer;

	struct gpio_desc *up;
	struct gpio_desc *down;
	struct gpio_desc *left;
	struct gpio_desc *right;

	/* Game Area */
	bool gameover;
	control_t button;
	int button_irq[4];
	uint32_t score;
	struct snake mySnake;
	struct food myFood;
};

static void ssd1306_write(struct ssd1306 *oled, uint8_t data, write_mode_t mode);
static void ssd1306_init(struct ssd1306 *oled);
static void ssd1306_clear(struct ssd1306 *oled);
static void ssd1306_goto_xy(struct ssd1306 *oled, uint8_t x, uint8_t y);
static void ssd1306_send_char(struct ssd1306 *oled, uint8_t data);
static void ssd1306_send_char_inv(struct ssd1306 *oled, uint8_t data);
static void ssd1306_send_string(struct ssd1306 *oled, uint8_t *str, color_t color);
static void ssd1306_go_to_next_line(struct ssd1306 *oled);
static int ssd1306_burst_write(struct ssd1306 *oled, const uint8_t *data, int len, write_mode_t mode);
static void animation(struct work_struct *work);
static void tmHandler(struct timer_list *tm);

irqreturn_t buttonHandler(int irq, void *dev_id)
{
	struct ssd1306 *oled = (struct ssd1306 *)dev_id;
	if (irq == oled->button_irq[0])
		oled->button = UP;
	else if (irq == oled->button_irq[1])
		oled->button = DOWN;
	else if (irq == oled->button_irq[2])
		oled->button = LEFT;
	else if (irq == oled->button_irq[3])
		oled->button = RIGHT;
	return IRQ_HANDLED;
}

/* Snake Game Area */
static void snake_update_score(struct ssd1306 *oled, uint32_t score);
static void snake_game_setup(struct ssd1306 *oled);
static void snake_game_draw(struct ssd1306 *oled);
static void snake_game_logic(struct ssd1306 *oled);
static uint8_t create_random_number(uint8_t MAX);

static int oled_probe(struct i2c_client *client)
{
	int i;
	struct device *dev = NULL;
	char *label[] = {
		"button-up",
		"button-down",
		"button-left",
		"button-right"};
	struct ssd1306 *oled = NULL;
	oled = devm_kzalloc(&client->dev, sizeof(*oled), GFP_KERNEL);
	if (!oled)
	{
		pr_err("kzalloc failed\n");
		return -1;
	}
	oled->client = client;
	dev = &client->dev;
	i2c_set_clientdata(client, oled);
	ssd1306_init(oled);
	ssd1306_goto_xy(oled, 0, 0);
	ssd1306_send_string(oled, "Snack Game", COLOR_WHITE);
	ssd1306_clear(oled);
	oled->up = gpiod_get_index(dev, "buttons", BUTTON_UP, GPIOD_IN);
	oled->down = gpiod_get_index(dev, "buttons", BUTTON_DOWN, GPIOD_IN);
	oled->left = gpiod_get_index(dev, "buttons", BUTTON_LEFT, GPIOD_IN);
	oled->right = gpiod_get_index(dev, "buttons", BUTTON_RIGHT, GPIOD_IN);
	oled->button_irq[0] = gpiod_to_irq(oled->up);
	oled->button_irq[1] = gpiod_to_irq(oled->down);
	oled->button_irq[2] = gpiod_to_irq(oled->left);
	oled->button_irq[3] = gpiod_to_irq(oled->right);

	for (i = 0; i < 4; i++)
	{
		if (request_irq(oled->button_irq[i], buttonHandler, IRQF_TRIGGER_FALLING | IRQF_SHARED, label[i], oled) < 0)
		{
			pr_err("request irq gpio %d failed!\n", i);
			return -ENODEV;
		}
	}

	snake_game_setup(oled);
	snake_game_draw(oled);

	INIT_WORK(&oled->workqueue, animation);
	timer_setup(&oled->my_timer, tmHandler, 0);
	oled->my_timer.expires = jiffies + HZ / 5;
	add_timer(&oled->my_timer);
	return 0;
}
static void oled_remove(struct i2c_client *client)
{
	int i;
	struct ssd1306 *oled = i2c_get_clientdata(client);
	if (!oled)
	{
		pr_err("Cannot get data\n");
	}
	else
	{
		for (i = 0; i < 4; i++)
			free_irq(oled->button_irq[i], oled);
		gpiod_put(oled->up);
		gpiod_put(oled->down);
		gpiod_put(oled->left);
		gpiod_put(oled->right);
		cancel_work_sync(&oled->workqueue);
		del_timer(&oled->my_timer);
		ssd1306_clear(oled);
		ssd1306_write(oled, 0xAE, COMMAND); // display off
	}
}
static const struct i2c_device_id oled_device_id[] = {
	{.name = "nam", 0},
	{}};
MODULE_DEVICE_TABLE(i2c, oled_device_id);
static const struct of_device_id oled_of_match_id[] = {
	{
		.compatible = "ssd1306-oled,nam",
	},
	{}};
MODULE_DEVICE_TABLE(of, oled_of_match_id);

static struct i2c_driver oled_driver = {
	.probe = oled_probe,
	.remove = oled_remove,
	.driver = {
		.name = "oled",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(oled_of_match_id),
	},
	.id_table = oled_device_id,
};

module_i2c_driver(oled_driver);

static void ssd1306_write(struct ssd1306 *oled, uint8_t data, write_mode_t mode)
{
	/*
	A control byte mainly consists of Co and D/C# bits following by six “0”
	Co | D/C | 000000
	Co bit is equal to 0
	*/
	uint8_t buff[2];
	if (mode == DATA)
		buff[0] = 0x40; // data
	else
		buff[0] = 0x00; // command
	buff[1] = data;
	i2c_master_send(oled->client, buff, 2);
}
static int ssd1306_burst_write(struct ssd1306 *oled, const uint8_t *data, int len, write_mode_t mode)
{
	int res;
	uint8_t *buff;
	buff = kmalloc(len + 1, GFP_KERNEL);
	if (!buff)
		return -1;
	memset(buff, 0, len + 1);
	if (mode == DATA)
		buff[0] = 0x40; // data
	else
		buff[0] = 0x00; // command
	memcpy(&buff[1], data, len);
	res = i2c_master_send(oled->client, buff, len + 1);
	kfree(buff);
	return res;
}
static void ssd1306_init(struct ssd1306 *oled)
{
	msleep(15);
	// set Osc Frequency
	ssd1306_write(oled, 0xD5, COMMAND);
	ssd1306_write(oled, 0x80, COMMAND);
	// set MUX Ratio
	ssd1306_write(oled, 0xA8, COMMAND);
	ssd1306_write(oled, 0x3F, COMMAND);
	// set display offset
	ssd1306_write(oled, 0xD3, COMMAND);
	ssd1306_write(oled, 0x00, COMMAND);
	// set display start line
	ssd1306_write(oled, 0x40, COMMAND);
	// Enable charge pump regulator
	ssd1306_write(oled, 0x8D, COMMAND);
	ssd1306_write(oled, 0x14, COMMAND);
	// Set memory addressing mode
	ssd1306_write(oled, 0x20, COMMAND);
	ssd1306_write(oled, 0x00, COMMAND);
	// Set segment remap with column address 0 mapped to segment 0
	ssd1306_write(oled, 0xA0, COMMAND);
	ssd1306_write(oled, 0xC0, COMMAND);
	// set COM Pin hardware configuration
	ssd1306_write(oled, 0xDA, COMMAND);
	ssd1306_write(oled, 0x12, COMMAND);
	// set contrast control
	ssd1306_write(oled, 0x81, COMMAND);
	ssd1306_write(oled, 0x7F, COMMAND);
	// Set pre-charge period
	ssd1306_write(oled, 0xD9, COMMAND);
	ssd1306_write(oled, 0xF1, COMMAND);
	// Set Vcomh deselect level
	ssd1306_write(oled, 0xDB, COMMAND);
	ssd1306_write(oled, 0x20, COMMAND);
	// disable entire display on
	ssd1306_write(oled, 0xA4, COMMAND);
	// set normal display
	ssd1306_write(oled, 0xA6, COMMAND); // A6 normal a7 inverse
	// set segment re-map
	ssd1306_write(oled, 0xA0, COMMAND);
	// deactive scroll
	ssd1306_write(oled, 0x2E, COMMAND);
	// display on
	ssd1306_write(oled, 0xAF, COMMAND);
	// clear screen
	ssd1306_clear(oled);
}
static void ssd1306_clear(struct ssd1306 *oled)
{
	int i;
	ssd1306_goto_xy(oled, 0, 1);
	for (i = 0; i < OLED_WIDTH * (OLED_HEIGHT / 8 - 1); i++)
		ssd1306_write(oled, 0, DATA);
}
static void ssd1306_goto_xy(struct ssd1306 *oled, uint8_t x, uint8_t y)
{
	ssd1306_write(oled, 0x21, COMMAND);
	ssd1306_write(oled, x * FONT_X, COMMAND);
	ssd1306_write(oled, OLED_WIDTH - 1, COMMAND);
	ssd1306_write(oled, 0x22, COMMAND);
	ssd1306_write(oled, y, COMMAND);
	ssd1306_write(oled, max_Y, COMMAND);
	oled->current_X = x;
	oled->current_Y = y;
}
static void ssd1306_send_char(struct ssd1306 *oled, uint8_t data)
{
	if (oled->current_X == max_X)
		ssd1306_go_to_next_line(oled);
	ssd1306_burst_write(oled, ssd1306_font[data - 32], FONT_X, DATA);
	oled->current_X++;
}
static void ssd1306_send_char_inv(struct ssd1306 *oled, uint8_t data)
{
	uint8_t i;
	uint8_t buff[FONT_X];
	if (oled->current_X == max_X)
		ssd1306_go_to_next_line(oled);
	for (i = 0; i < FONT_X; i++)
		buff[i] = ~ssd1306_font[data - 32][i];
	ssd1306_burst_write(oled, buff, FONT_X, DATA);
	oled->current_X++;
}
static void ssd1306_send_string(struct ssd1306 *oled, uint8_t *str, color_t color)
{
	while (*str)
	{
		if (color == COLOR_WHITE)
			ssd1306_send_char(oled, *str++);
		else
			ssd1306_send_char_inv(oled, *str++);
	}
}
static void ssd1306_go_to_next_line(struct ssd1306 *oled)
{
	oled->current_Y = (oled->current_Y == max_Y) ? 0 : (oled->current_Y + 1);
	ssd1306_goto_xy(oled, 0, oled->current_Y);
}

static void animation(struct work_struct *work)
{
	struct ssd1306 *oled = container_of(work, struct ssd1306, workqueue);
	if (oled)
	{
		if (oled->gameover == FALSE)
		{
			snake_game_draw(oled);
			snake_game_logic(oled);
		}
		else
		{
			oled->button = PAUSE;
			ssd1306_goto_xy(oled, 5, 4);
			ssd1306_send_string(oled, "Game Over!", COLOR_WHITE);
		}
	}
	mod_timer(&oled->my_timer, jiffies + HZ / 5);
}
static void tmHandler(struct timer_list *tm)
{
	struct ssd1306 *oled = container_of(tm, struct ssd1306, my_timer);
	if (oled)
		schedule_work(&oled->workqueue);
}
static uint8_t create_random_number(uint8_t MAX)
{
	uint8_t random_number;
	get_random_bytes(&random_number, sizeof(random_number));
	return random_number % MAX;
}
/* Snake Game */
static void snake_game_setup(struct ssd1306 *oled)
{
	oled->button = PAUSE;
	oled->gameover = FALSE;
	oled->mySnake.x = max_X / 2;
	oled->mySnake.y = max_Y / 2;
	while (!oled->myFood.x)
		oled->myFood.x = create_random_number(max_X) - 1;
	while (!oled->myFood.y)
		oled->myFood.y = create_random_number(max_Y - 1);
	snake_update_score(oled, 0);
}
static void snake_update_score(struct ssd1306 *oled, uint32_t score)
{
	int i;
	char scoreBuffer[20];
	ssd1306_goto_xy(oled, 0, 0);
	for (i = 0; i < 20 * FONT_X; i++)
		ssd1306_write(oled, 0, DATA);
	memset(scoreBuffer, 0, sizeof(scoreBuffer));
	sprintf(scoreBuffer, "Score: %d", score);
	ssd1306_goto_xy(oled, 0, 0);
	ssd1306_send_string(oled, scoreBuffer, COLOR_WHITE);
}
static void snake_game_draw(struct ssd1306 *oled)
{
	int i, j;
	ssd1306_clear(oled);
	for (i = 0; i < max_Y; i++)
	{
		for (j = 0; j < max_X; j++)
		{
			if (i == 0 || i == max_Y - 1 || j == 0 || j == max_X - 1)
				ssd1306_send_char(oled, '+');
			else
			{
				if (i == oled->mySnake.y && j == oled->mySnake.x)
					ssd1306_send_char(oled, '0');
				else if (i == oled->myFood.y && j == oled->myFood.x)
					ssd1306_send_char(oled, '*');
				else
					ssd1306_send_char(oled, ' ');
			}
		}
		ssd1306_go_to_next_line(oled);
	}
	snake_update_score(oled, oled->score);
}
static void snake_game_logic(struct ssd1306 *oled)
{
	switch (oled->button)
	{
	case UP:
		oled->mySnake.y--;
		break;
	case DOWN:
		oled->mySnake.y++;
		break;
	case LEFT:
		oled->mySnake.x--;
		break;
	case RIGHT:
		oled->mySnake.x++;
		break;
	default:
		break;
	}
	if (oled->mySnake.x < 0 || oled->mySnake.x > max_X || oled->mySnake.y < 0 || oled->mySnake.y > max_Y)
		oled->gameover = TRUE;
	if (oled->mySnake.x == oled->myFood.x && oled->mySnake.y == oled->myFood.y)
	{
		oled->myFood.x = 0;
		oled->myFood.y = 0;
		while (!oled->myFood.x)
			oled->myFood.x = create_random_number(max_X - 1);
		while (!oled->myFood.y)
			oled->myFood.y = create_random_number(max_Y - 1);
		oled->score += 10;
	}
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("20021163@vnu.edu.vn");
MODULE_DESCRIPTION("Snake Game with oled ssd1306");
MODULE_VERSION("1.0");
